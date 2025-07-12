#include "protocol.hpp"
#include "completion.hpp"
#include "document_symbol.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

int Protocol::handle(nlohmann::json& req)
{
    nlohmann::json resp;
    std::cerr << "start handle protocol req: \n" << req.dump(4) << std::endl;

    std::string method = req["method"];
    if (method != "initialize" && !init_) {
        std::cerr << "received request but server is uninitialized." << std::endl;
        return 0;
    }

    if (method == "initialize") {
        initialize_(req);
    } else if (method == "initialized") {
        return 0;
    } else if (method == "workspace/didChangeConfiguration") {
        return 0;
    } else if (method == "textDocument/didOpen") {
        did_open_(req);
    } else if (method == "textDocument/definition") {
        definition_(req);
    } else if (method == "textDocument/didChange") {
        did_change_(req);
    } else if (method == "textDocument/completion") {
        completion_(req);
    } else if (method == "textDocument/didSave") {
        did_save_(req);
    } else if (method == "textDocument/documentSymbol") {
        document_symbol_(req);
    }

    return 0;
}

void Protocol::make_response_(nlohmann::json& req, nlohmann::json* result)
{
    nlohmann::json body;
    if (result) {
        body = {
            {"jsonrpc", "2.0"},
            {"id", req["id"]},
            {"result", *result},
        };
    } else if (result->is_array()) {
        body = nlohmann::json::parse(R"(
			{
				"jsonrpc": "2.0",
				"result": [] 
			}
		)");
        body["id"] = req["id"];
    } else {
        body = nlohmann::json::parse(R"(
			{
				"jsonrpc": "2.0",
				"result": null
			}
		)");
        body["id"] = req["id"];
    }

    send_to_client_(body);
}

void Protocol::initialize_(nlohmann::json& req)
{
    auto result = nlohmann::json::parse(R"(
	{
		"capabilities": {
			"textDocumentSync": {
				"openClose": true,
				"change": 1,
				"save": true,
				"willSave": false 
			},
			"completionProvider": {
				"triggerCharacters": ["."],
				"resolveProvider": false,
				"completionItem": {
					"labelDetailsSupport": true
				}
			},
			"hoverProvider": false,
			"signatureHelpProvider": {
				"triggerCharacters": []
			},
			"declarationProvider": false,
			"definitionProvider": true,
			"typeDefinitionProvider": false,
			"implementationProvider": false,
			"referencesProvider": true,
			"documentHighlightProvider": false,
			"documentSymbolProvider": true,
			"codeActionProvider": false,
			"codeLensProvider": false,
			"documentLinkProvider": false,
			"colorProvider": false,
			"documentFormattingProvider": false,
			"documentRangeFormattingProvider": false,
			"documentOnTypeFormattingProvider": false,
			"renameProvider": false,
			"foldingRangeProvider": false,
			"executeCommandProvider": false,
			"selectionRangeProvider": false,
			"linkedEditingRangeProvider": false,
			"callHierarchyProvider": false,
			"semanticTokensProvider": false,
			"monikerProvider": false,
			"typeHierarchyProvider": false,
			"inlineValueProvider": false,
			"inlayHintProvider": false,
			"workspaceSymbolProvider": false
		}
	}
	)");

    // std::cerr << "build reqsp capabilities: " << std::endl << result.dump() << std::endl;
    nlohmann::json params = req["params"];
    workspace_.init(params["rootPath"]);

    init_ = true;
    // std::cerr << "init workspace at root: " << workspace_.get_root() << std::endl;
    make_response_(req, &result);
}

void Protocol::did_open_(nlohmann::json& req)
{
    if (!init_) {
        fprintf(stderr, "server is uninitialized\n");
        return;
    }

    auto& params = req["params"];
    auto& textDoc = params["textDocument"];
    std::string uri = textDoc["uri"];
    int version = textDoc["version"];
    std::string source = textDoc["text"];
    Doc doc(uri, version, source);
    if (!doc.parse({workspace_.get_root()})) {
        publish_diagnostics(doc.info_log());
    } else {
        publish_clear_diagnostics(uri);
    }
    workspace_.add_doc(std::move(doc));
}

void Protocol::did_save_(nlohmann::json& req)
{
    auto& params = req["params"];
    std::string uri = params["textDocument"]["uri"];
    int version = params["textDocument"]["version"];

    auto [ret, doc] = workspace_.save_doc(uri, version);
    if (doc) {
        if (ret)
            publish_clear_diagnostics(uri);
        else
            publish_diagnostics(doc->info_log());
    }
}

void Protocol::completion_(nlohmann::json& req)
{
    auto& params = req["params"];
    // int triggerKind = params["context"]["triggerKind"];
    int line = params["position"]["line"];
    int col = params["position"]["character"];
    std::string uri = params["textDocument"]["uri"];

    auto doc = workspace_.get_doc(uri);
    nlohmann::json completion_items;
    if (!doc) {
        make_response_(req, &completion_items);
        return;
    }

    auto get_words = [this, &uri, line, col](int tok) { return workspace_.get_sentence(uri, line, col, tok); };
    std::vector<std::string> sentence = {get_words(';'), get_words('\n'), get_words('('), get_words('['),
                                         get_words('{'), get_words(' '),  get_words('#')};

    std::set<std::string> uniq_sentence(sentence.cbegin(), sentence.cend());

    std::vector<std::string> anon_prefix;
    for (auto sym : doc->lookup_symbols_by_prefix(nullptr, "anon@")) {
        if (!sym->isStruct())
            continue;
        anon_prefix.push_back(sym->getName().c_str());
    }

    CompletionResultSet complete_results;
    for (auto& word : uniq_sentence) {
        completion(*doc, {}, word, line, col, complete_results);
        if (!isalpha(word.front()) && word.front() != '_') {
            continue;
        }

        for (auto const& prefix : anon_prefix) {
            completion(*doc, prefix, word, line, col, complete_results);
        }
    }

    auto unique_candidates = [](auto& results) {
        std::set<std::string> item_set;
        std::vector<CompletionResult> final_results;
        for (auto& item : results) {
            if (item_set.count(item.insert_text) <= 0) {
                item_set.insert(item.insert_text);
                final_results.push_back(item);
            }
        }

        results.swap(final_results);
    };

    unique_candidates(complete_results.variables);
    unique_candidates(complete_results.funcs);
    unique_candidates(complete_results.types);
    unique_candidates(complete_results.keywords);
    unique_candidates(complete_results.builtins);

    std::vector<CompletionResult> results;

    auto append_to_results = [&results](auto const& candidates) {
        std::copy(candidates.cbegin(), candidates.cend(), std::back_inserter(results));
    };

    append_to_results(complete_results.variables);
    append_to_results(complete_results.funcs);
    append_to_results(complete_results.types);
    append_to_results(complete_results.keywords);
    append_to_results(complete_results.builtins);

    if (results.size() >= 200) {
        results.resize(200);
    }

    for (auto const& result : results) {
        completion_items.push_back(result.json());
    }

    make_response_(req, &completion_items);
}

void Protocol::definition_(nlohmann::json& req)
{
    if (!init_) {
        fprintf(stderr, "server is uninitialized\n");
        return;
    }
    std::cerr << "handle goto definition" << std::endl;

    auto& params = req["params"];
    std::string uri = params["textDocument"]["uri"];
    int col = params["position"]["character"];
    int line = params["position"]["line"];

    std::cerr << "target sym at " << line << ":" << col << std::endl;
    auto loc = workspace_.locate_symbol_def(uri, line + 1, col + 1);

    if (loc.name) {
        nlohmann::json result;
        nlohmann::json start = {{"line", loc.line - 1}, {"character", loc.column - 1}};
        result["uri"] = loc.name->c_str();
        result["range"] = {{"start", start}, {"end", start}};
        make_response_(req, &result);
    } else {
        make_response_(req, nullptr);
    }
}

void Protocol::did_change_(nlohmann::json& req)
{
    auto& params = req["params"];
    auto& textDoc = params["textDocument"];
    std::string uri = textDoc["uri"];
    int version = textDoc["version"];
    std::string source = params["contentChanges"][0]["text"];
    workspace_.update_doc(uri, version, source);
}

void Protocol::document_symbol_(nlohmann::json& req)
{
    auto& params = req["params"];
    std::string uri = params["textDocument"]["uri"];
    auto* doc = workspace_.get_doc(uri);
    auto arr = nlohmann::json::array({});
    if (!doc) {
        make_response_(req, &arr);
    }

    auto symbols = document_symbol(doc);

    for (auto const& s : symbols) {
        arr.push_back(s.json());
    }

    make_response_(req, &arr);
}

void Protocol::publish_(std::string const& method, nlohmann::json* params)
{
    nlohmann::json body;
    if (!params) {
        body = nlohmann::json::parse(R"(
			{
				"params": {}
			}
		)");
        body["method"] = method;
    } else {
        body = {{"method", method}, {"params", *params}};
    }

    send_to_client_(body);
}

void Protocol::publish_diagnostics(std::string const& error)
{
    std::stringstream ss(error);
    std::string line;
    std::map<std::string, nlohmann::json> diagnostics;

    while (std::getline(ss, line)) {
        std::smatch result;
        std::regex pattern("ERROR: (file:///.*):([0-9]+): (.*)");
        if (std::regex_match(line, result, pattern)) {
            std::string uri = result[1].str();
            int row = std::atoi(result[2].str().c_str()) - 1;
            std::string message = result[3].str();
            nlohmann::json start = {{"line", row}, {"character", 1}};
            nlohmann::json diagnostic = {{"range", {{"start", start}, {"end", start}}}, {"message", message}};
            diagnostics[uri].push_back(diagnostic);
        }
    }

    for (auto& [uri, diagnostic] : diagnostics) {
        nlohmann::json body = {{"uri", uri}, {"diagnostics", diagnostic}};
        publish_("textDocument/publishDiagnostics", &body);
    }
}

void Protocol::publish_clear_diagnostics(const std::string& uri)
{
    nlohmann::json body = nlohmann::json::parse(R"(
	{
		"diagnostics": []
	}
	)");
    body["uri"] = uri;
    publish_("textDocument/publishDiagnostics", &body);
}

void Protocol::send_to_client_(nlohmann::json& content)
{
    std::string body_str = content.dump();

    std::string header;
    header.append("Content-Length: ");
    header.append(std::to_string(body_str.size()) + "\r\n");
    header.append("Content-Type: application/vscode-jsonrpc;charset=utf-8\r\n");
    header.append("\r\n");
    header.append(body_str);
    std::cerr << "resp to client: \n" << header << std::endl;
    std::cout << header;
    std::flush(std::cout);
}
