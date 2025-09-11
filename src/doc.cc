#include "doc.hpp"
#include "StandAlone/DirStackFileIncluder.h"
#include "args.hpp"
#include "extractors.hpp"
#include "glslang/Include/intermediate.h"
#include "glslang/MachineIndependent/SymbolTable.h"
#include "glslang/MachineIndependent/localintermediate.h"
#include "parser.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

extern int yylex(YYSTYPE*, glslang::TParseContext&);

Doc::Doc() { resource_ = nullptr; }

Doc::~Doc() { release_(); }

Doc::Doc(std::string const& uri, const int version, std::string const& text, CompileOption const& option)
    : option_(option)
{
    resource_ = new __Resource;
    resource_->ref = 1;
    resource_->uri = uri;
    resource_->version = version;

    infer_language_();
    set_text(text);
    resource_->shader = nullptr;
}

void Doc::set_text(std::string const& text)
{
    if (!resource_)
        return;
    std::stringstream ss(text);
    std::string line;

    resource_->lines_.clear();
    while (std::getline(ss, line)) {
        resource_->lines_.emplace_back(std::move(line));
    }

    resource_->text_ = text;
    compute_inactive_blocks_();
}

Doc::Doc(const Doc& rhs)
{
    release_();
    resource_ = rhs.resource_;
    resource_->ref += 1;
    option_ = rhs.option_;
}

Doc::Doc(Doc&& rhs)
{
    release_();
    resource_ = rhs.resource_;
    rhs.resource_ = nullptr;
    option_ = rhs.option_;
}

Doc& Doc::operator=(const Doc& rhs)
{
    release_();
    resource_ = rhs.resource_;
    resource_->ref += 1;
    option_ = rhs.option_;
    return *this;
}

Doc& Doc::operator=(Doc&& rhs)
{
    release_();
    resource_ = rhs.resource_;
    rhs.resource_ = nullptr;
    option_ = rhs.option_;
    return *this;
}

void Doc::infer_language_()
{
    if (!resource_)
        return;

    std::filesystem::path p(uri());
    if (p.has_extension()) {
        auto extension = p.extension();
        auto s = extension.string();
        resource_->language = map_to_stage(s.substr(1));
    } else {
        resource_->language = EShLangCount;
    }
}

void Doc::release_()
{
    if (resource_) {
        if (resource_->ref == 1) {
            delete resource_;
        } else {
            resource_->ref -= 1;
        }
    }
}

class BuiltinSymbolTable : public glslang::TSymbolTable {
public:
    void get_all_symbols(std::vector<glslang::TSymbol*>& symbols)
    {
        for (int level = currentLevel(); level >= 0; --level) {
            glslang::TSymbolTableLevel* table_level = table[level];
            for (auto pos = table_level->get_level().begin(); pos != table_level->get_level().end(); ++pos) {
                symbols.push_back(pos->second);
                if (auto var = pos->second->getAsVariable()) {
                    auto loc = var->getLoc();
                    const auto& type = var->getType();
                    if (loc.name && type.isStruct()) {
                    }
                }
            }
        }
    }
};

std::unique_ptr<glslang::TShader> Doc::create_shader()
{
    CompileOption compile_option = option_;
    auto stage = compile_option.shader_stage == EShLangCount ? language() : compile_option.shader_stage;

    if (stage == EShLangCount) {
        fprintf(stderr, "unkown stage: %s\n", uri().c_str());
        return nullptr;
    }

    auto p = std::make_unique<glslang::TShader>(stage);
    auto& shader = *p;
    shader.setEntryPoint(compile_option.entrypoint.c_str());

    shader.setAutoMapBindings(compile_option.auto_bind_uniforms);
    if (compile_option.auto_combinded_image_sampler) {
        shader.setTextureSamplerTransformMode(EShTexSampTransUpgradeTextureRemoveSampler);
    }

    shader.setShiftImageBinding(compile_option.image_binding_base[stage]);
    shader.setShiftSamplerBinding(compile_option.sampler_binding_base[stage]);
    shader.setShiftTextureBinding(compile_option.texture_binding_base[stage]);
    shader.setShiftUboBinding(compile_option.ubo_binding_base[stage]);
    shader.setShiftSsboBinding(compile_option.ssbo_binding_base[stage]);
    shader.setShiftUavBinding(compile_option.uav_binding_base[stage]);

    shader.setEnvClient(compile_option.client, compile_option.client_version);
    shader.setEnvTarget(glslang::EShTargetSpv, compile_option.target_spv);
    glslang::EShSource language = compile_option.language;

    // This option will only be used if the Vulkan client is used.
    // If new versions of GL_KHR_vulkan_glsl come out, it would make sense to
    // let callers specify which version to use. For now, just use 100.
    shader.setEnvInput(language, stage, compile_option.client, compile_option.client_version);
    shader.setEnvInputVulkanRulesRelaxed();
    shader.setInvertY(compile_option.invert_y);
    shader.setNanMinMaxClamp(false);

    auto& shader_strings = resource_->text_;
    const char* shader_source = shader_strings.data();
    const int shader_lengths = (int)shader_strings.size();
    const char* string_names = resource_->uri.data();
    shader.setStringsWithLengthsAndNames(&shader_source, &shader_lengths, &string_names, 1);

    return std::move(p);
}

bool Doc::parse()
{
    if (!resource_)
        return false;

    auto* resource = new Doc::__Resource;
    resource->shader = create_shader();

    resource->nodes_by_line.clear();
    resource->globals.clear();
    resource->func_defs.clear();
    resource->userdef_types.clear();
    resource->builtins.clear();

    auto& shader = *resource->shader;

    std::string preambles;
    for (auto const& [k, v] : option_.macros) {
        preambles.append("#define " + k + " " + v + "\n");
    }

    const std::string pound_extension = "#extension GL_GOOGLE_include_directive : enable\n";
    preambles += pound_extension;

    shader.setPreamble(preambles.c_str());
    shader.setDebugInfo(true);

    BuiltinSymbolTable builtin_symbol_table;
    shader.setBuiltinSymbolTable(&builtin_symbol_table);

    DirStackFileIncluder includer;
    for (auto& d : option_.include_dirs) {
        includer.pushExternalLocalDirectory(d);
    }

    const EShMessages rules = static_cast<EShMessages>(EShMsgCascadingErrors | EShMsgSpvRules | EShMsgVulkanRules);
    auto default_version_ = option_.version;
    auto default_profile_ = option_.profile;
    auto force_version_profile_ = false;

    bool success = false;

    success = shader.parse(&kDefaultTBuiltInResource, default_version_, default_profile_, force_version_profile_, false,
                           rules, includer);
    if (!success) {
        resource_->info_log = shader.getInfoLog();
        delete resource;
        return false;
    }

    fprintf(stderr, "%s\n", shader.getInfoDebugLog());
    auto* interm = shader.getIntermediate();

#if 0
    {
        TInfoSink sink;
        interm->output(sink, true);
        std::cerr << sink.info.c_str() << std::endl;
        std::cerr << sink.debug.c_str() << std::endl;
    }
#endif

    DocInfoExtractor visitor;
    interm->getTreeRoot()->traverse(&visitor);

    for (auto& s : visitor.globals) {
        auto loc = s->getLoc();
        fprintf(stderr, "global symbol %s define at %s:%d:%d\n", s->getName().c_str(), loc.getFilename(), loc.line,
                loc.column);
        resource->globals.push_back(s);
    }

    for (auto& t : visitor.userdef_types) {
        auto loc = t->getLoc();
        auto const& type = t->getType();
        fprintf(stderr, "user def type %s: %s define at %s:%d:%d\n", type.getTypeName().c_str(),
                type.getCompleteString(true, false, false).c_str(), loc.getFilename(), loc.line, loc.column);
    }

    fprintf(stderr, "DocInfoExtractor found %zu function def\n", visitor.funcs.size());
    resource->globals.swap(visitor.globals);
    resource->func_defs.swap(visitor.funcs);
    resource->nodes_by_line.swap(visitor.nodes_by_line);
    resource->userdef_types.swap(visitor.userdef_types);
    builtin_symbol_table.get_all_symbols(resource->builtins);

    resource->uri = resource_->uri;
    resource->version = resource_->version;
    resource->text_ = std::move(resource_->text_);
    resource->lines_ = std::move(resource_->lines_);
    resource->language = resource_->language;

    release_();
    resource_ = resource;
	compute_inactive_blocks_();
    return true;
}

void Doc::tokenize_(CompileOption const& options)
{
    // tokenize
    auto default_version_ = options.version;
    auto default_profile_ = options.profile;
    auto default_spv = glslang::SpvVersion();
    auto parser = create_parser(default_version_, default_profile_, language(), default_spv, "main");

    size_t len = resource_->text_.size();
    const char* shader_source = resource_->text_.data();
    glslang::TInputScanner scanner(1, &shader_source, &len);

    parser->ppcontext->setInput(scanner, false);
    parser->parse_context->setScanner(&scanner);

    while (true) {
        YYSTYPE stype;
        int tok = yylex(&stype, *parser->parse_context);
        if (!tok) {
            break;
        }

        auto line = stype.lex.loc.line;
        resource_->tokens_by_line[line].push_back({tok, stype.lex});
    }
}

static Doc::LookupResult lookup_binop(glslang::TIntermBinary* binary, const int line, const int col)
{
    if (binary->getOp() != glslang::EOpIndexDirectStruct) {
        return {Doc::LookupResult::Kind::ERROR};
    }

    auto loc = binary->getLoc();
    if (loc.line != line) {
        return {Doc::LookupResult::Kind::ERROR};
    }

    auto* left = binary->getLeft();
    auto* right = binary->getRight();
    const auto rloc = right->getLoc();
    const auto lloc = left->getLoc();

    bool isref = left->getType().isReference();
    const auto* members = isref ? left->getType().getReferentType()->getStruct() : left->getType().getStruct();
    const auto index = right->getAsConstantUnion()->getConstArray()[0].getIConst();
    const auto field = (*members)[index];

    if (rloc.column <= col && col <= rloc.column + field.type->getFieldName().size()) {
        return {Doc::LookupResult::Kind::FIELD, nullptr, field, nullptr};
    }

    if (auto* left_sym = left->getAsSymbolNode()) {
        if (lloc.line == line && lloc.column <= col && col <= lloc.column + left_sym->getName().size()) {
            return {Doc::LookupResult::Kind::SYMBOL, left_sym, {}, nullptr};
        }
    }

    return {Doc::LookupResult::Kind::ERROR};
}

void Doc::compute_inactive_blocks_()
{

    auto p = create_shader();
    auto& shader = *p;
    std::map<std::string, std::map<int, int>> pp_cond_res;
    shader.setPpCondRes(&pp_cond_res);

    std::string preambles;
    for (auto const& [k, v] : option_.macros) {
        preambles.append("#define " + k + " " + v + "\n");
    }

    const std::string pound_extension = "#extension GL_GOOGLE_include_directive : enable\n";
    preambles += pound_extension;

    shader.setPreamble(preambles.c_str());
    shader.setDebugInfo(true);

    const EShMessages rules = static_cast<EShMessages>(EShMsgCascadingErrors | EShMsgSpvRules | EShMsgVulkanRules);
    auto default_version_ = option_.version;
    auto default_profile_ = option_.profile;
    auto force_version_profile_ = false;

    DirStackFileIncluder includer;
    for (auto& d : option_.include_dirs) {
        includer.pushExternalLocalDirectory(d);
    }

    std::string preprocessed_text;
    bool success = shader.preprocess(&kDefaultTBuiltInResource, default_version_, default_profile_,
                                     force_version_profile_, false, rules, &preprocessed_text, includer);

    if (!success)
        return;

    auto& file_cond_res = pp_cond_res[uri()];
    ComputeInactiveHelper helper(resource_->lines_, file_cond_res);
    resource_->inactive_blocks_ = helper.inactive();
}

Doc::LookupResult Doc::lookup_node_in_struct(const int line, const int col)
{
    auto* func = lookup_func_by_line(line);
    auto const& tokens = resource_->tokens_by_line[line];

    auto lookup_member_type_fn = [line, col, &tokens](const glslang::TType& type) {
        if (!type.isStruct()) {
            return Doc::LookupResult{Doc::LookupResult::Kind::ERROR, nullptr, {}, nullptr};
        }

        auto const& members = *type.getStruct();

        for (size_t i = 0; i < members.size(); ++i) {
            auto const& member = members[i];
            if (member.loc.line != line) {
                continue;
            }

            if (col >= member.loc.column) {
                continue;
            }

            auto pos = std::find_if(tokens.cbegin(), tokens.cend(), [&member](Token const& tok) {
                if (tok.lex.loc.column == member.loc.column) {
                    return true;
                }
                return false;
            });

            if (pos == tokens.cbegin()) {
                continue;
            }

            --pos;

            auto const& type_tok = *pos;
            if (type_tok.tok != IDENTIFIER && type_tok.tok != TYPE_NAME) {
                continue;
            }

            const int type_start_col = type_tok.lex.loc.column;
            const int type_end_col = type_start_col + type_tok.lex.string->size();
            if (col >= type_start_col && col <= type_end_col) {
                return Doc::LookupResult{Doc::LookupResult::Kind::TYPE, nullptr, {}, member.type};
            }
        }

        return Doc::LookupResult{Doc::LookupResult::Kind::ERROR, nullptr, {}, nullptr};
    };

    if (func) {
        for (auto* def : func->userdef_types) {
            const auto& type = def->getType();

            auto result = lookup_member_type_fn(type);
            if (result.kind != LookupResult::Kind::ERROR) {
                return result;
            }
        }
    }

    for (auto* global : resource_->globals) {
        auto result = lookup_member_type_fn(global->getType());
        if (result.kind != LookupResult::Kind::ERROR) {
            return result;
        }
    }

    return Doc::LookupResult{Doc::LookupResult::Kind::ERROR, nullptr, {}, nullptr};
}

std::vector<Doc::LookupResult> Doc::lookup_nodes_at(const int line, const int col)
{
    if (!resource_)
        return {};
    std::vector<LookupResult> result;

    if (resource_->nodes_by_line.count(line) <= 0) {
        auto ty = lookup_node_in_struct(line, col);
        if (ty.kind != LookupResult::Kind::ERROR) {
            result.push_back(ty);
        }

        return result;
    }

    auto& nodes = resource_->nodes_by_line[line];

    for (auto* node : nodes) {
        if (auto sym = node->getAsSymbolNode()) {
            auto loc = sym->getLoc();
            auto endcol = loc.column + sym->getName().length();
            if (line == loc.line && loc.column <= col && col <= endcol) {
                result.push_back({LookupResult::Kind::SYMBOL, sym, {}, nullptr});
            }
        } else if (auto binop = node->getAsBinaryNode()) {
            auto bin_result = lookup_binop(binop, line, col);
            if (bin_result.kind != LookupResult::Kind::ERROR) {
                result.push_back(bin_result);
            }
        } else if (auto unary = node->getAsUnaryNode()) {
            if (unary->getOp() != glslang::EOpDeclare) {
                continue;
            }

            auto const& ty = unary->getType();
            if (!ty.isStruct()) {
                continue;
            }

            auto loc = unary->getLoc();
            auto startcol = loc.column;
            auto endcol = startcol + ty.getTypeName().size();

            if (line == loc.line && startcol <= col && col <= endcol) {
                result.push_back({LookupResult::Kind::TYPE, nullptr, {}, &ty});
            }
        }
    }

    return result;
}

glslang::TSourceLoc Doc::locate_symbol_def(Doc::FunctionDefDesc* func, glslang::TIntermSymbol* target)
{
    if (func) {
        for (auto sym : func->local_defs) {
            if (sym->getId() == target->getId()) {
                return sym->getLoc();
            }
        }
    }

    for (auto global : resource_->globals) {
        if (target->getId() == global->getId()) {
            return global->getLoc();
        }
    }

    return {nullptr, 0, 0};
}

glslang::TSourceLoc Doc::locate_userdef_type(int line, const glslang::TType* ty)
{
    if (!resource_)
        return {};

    auto* func = lookup_func_by_line(line);
    if (func) {
        for (auto* def : func->userdef_types) {
            if (def->getType().getTypeName() == ty->getTypeName()) {
                return def->getLoc();
            }
        }
    }

    for (auto* def : resource_->userdef_types) {
        if (def->getType().getTypeName() == ty->getTypeName()) {
            return def->getLoc();
        }
    }

    return {nullptr, 0, 0};
}

std::vector<glslang::TIntermSymbol*> Doc::lookup_symbols_by_prefix(Doc::FunctionDefDesc* func,
                                                                   std::string const& prefix)
{
    if (!resource_)
        return {};
    std::vector<glslang::TIntermSymbol*> symbols;
    auto match_fn = [&prefix, &symbols](auto vars) {
        for (auto& sym : vars) {
            std::string name = sym->getName().c_str();
            if (name.size() < prefix.size()) {
                continue;
            }

            std::string sub = name.substr(0, prefix.size()).c_str();
            if (sub == prefix) {
                symbols.push_back(sym);
            }
        }
    };

    match_fn(resource_->globals);
    if (func) {
        match_fn(func->args);
        match_fn(func->local_defs);
    }

    return symbols;
}

std::vector<glslang::TSymbol*> Doc::lookup_builtin_symbols_by_prefix(std::string const& prefix, bool fullname)
{
    if (!resource_)
        return {};
    auto match_fn = [&prefix, fullname](std::string const& name) {
        if (prefix.empty())
            return true;

        return fullname ? prefix == name : prefix == name.substr(0, prefix.size());
    };

    std::vector<glslang::TSymbol*> results;
    for (auto* sym : resource_->builtins) {
        if (match_fn(sym->getName().c_str())) {
            results.push_back(sym);
        }
    }

    return results;
}

glslang::TIntermSymbol* Doc::lookup_symbol_by_name(Doc::FunctionDefDesc* func, std::string const& name)
{
    if (func) {
        for (auto* def : func->local_defs) {
            if (name == def->getName().c_str()) {
                return def;
            }
        }

        for (auto* def : func->args) {
            if (name == def->getName().c_str()) {
                return def;
            }
        }
    }

    for (auto& sym : resource_->globals) {
        if (name == sym->getName().c_str()) {
            return sym;
        }
    }

    return nullptr;
}

Doc::FunctionDefDesc* Doc::lookup_func_by_line(int line)
{
    if (!resource_)
        return nullptr;
    for (auto& func : resource_->func_defs) {
        if (func.start.line <= line && func.end.line >= line) {
            return &func;
        }
    }
    return nullptr;
}
