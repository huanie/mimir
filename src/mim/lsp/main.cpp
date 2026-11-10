#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

#ifdef _WIN32
#    include <io.h>
#else
#    include <unistd.h>
#endif

#include <fe/cli.h>
#include <fe/term.h>
#include <fe/utf8.h>
#include <lsp/io/standard_io.h>
#include <lsp/server_endpoint.h>

#include "mim/driver.h"
#include "mim/manifest.h"
#include "mim/plugin.h"

#include "mim/ast/ast.h"
#include "mim/ast/parser.h"
#include "mim/phase/optimize.h"

namespace mim {
namespace {

using Diagnostics = fe::StrMap<std::vector<lsp::Diagnostic>>;

lsp::DocumentUri uri(const fs::path& path) {
    return lsp::Uri::fileUriFromPath(fs::absolute(path).lexically_normal().generic_string());
}

lsp::Position position(const fe::Src& src, fe::Pos pos, lsp::PositionEncodingKind encoding) {
    assert(src.contains(pos));
    auto [row, col] = src.rowcol(pos);
    if (!row || !col) return {};
    auto line    = src.line(row);
    auto utf16   = uint32_t(0);
    size_t bytes = 0;
    for (uint32_t point = 1; bytes < line.size() && point < col; ++point) {
        auto cp = fe::utf8::decode(line, bytes);
        utf16 += cp != fe::utf8::Invalid && cp > 0xffff ? 2 : 1;
    }
    return {.line = row - 1, .character = encoding == lsp::PositionEncodingKind::UTF8 ? uint32_t(bytes) : utf16};
}

lsp::Location location(Loc loc, const lsp::DocumentUri& fallback, lsp::PositionEncodingKind encoding) {
    if (loc && loc.src)
        return {
            .uri   = uri(loc.src->path()),
            .range = {.start = position(*loc.src, loc.begin, encoding), .end = position(*loc.src, loc.end, encoding)}
        };
    return {.uri = fallback, .range = {}};
}

class DiagnosticSink : public Diag {
public:
    DiagnosticSink(const Driver& driver,
                   Diagnostics& diagnostics,
                   lsp::DocumentUri fallback,
                   lsp::PositionEncodingKind encoding)
        : Diag(driver)
        , diagnostics_(diagnostics)
        , fallback_(std::move(fallback))
        , encoding_(encoding) {}

    std::string render(const std::function<std::string()>& fmt) const override {
        auto mode = fe::term::ScopedMode(fe::term::Mode::Never);
        return Diag::render(fmt);
    }

    void header(std::ostream&, Loc loc, Tag tag, std::string_view str) const override {
        auto at             = location(loc, fallback_, encoding_);
        auto& diagnostic    = diagnostics_[at.uri.toString()].emplace_back();
        diagnostic.range    = at.range;
        diagnostic.message  = std::string(str);
        diagnostic.severity = tag == Tag::W ? lsp::DiagnosticSeverity::Warning : lsp::DiagnosticSeverity::Error;
        diagnostic.source   = "mim";
        current_            = &diagnostic;
        current_uri_        = at.uri;
    }

    void note(std::ostream&, Loc loc, std::string_view str) const override {
        if (current_) {
            if (!current_->relatedInformation) current_->relatedInformation.emplace();
            current_->relatedInformation->push_back(
                {.location = location(loc, current_uri_, encoding_), .message = std::string(str)});
            if (!loc || !loc.src) std::get<lsp::String>(current_->message) += std::format("\n= note: {}", str);
        }
    }

    void snippet(std::ostream&, Loc, Tag) const override {}
    void summary(std::ostream&, size_t, size_t, bool) const override {}

private:
    Diagnostics& diagnostics_;
    lsp::DocumentUri fallback_;
    lsp::PositionEncodingKind encoding_;
    mutable lsp::Diagnostic* current_ = nullptr;
    mutable lsp::DocumentUri current_uri_;
};

class RedirectOutput {
public:
    RedirectOutput() {
        std::cout.flush();
        std::fflush(stdout);
        saved_ = dup(fileno(stdout));
        if (saved_ < 0) throw std::system_error(errno, std::generic_category(), "saving stdout");
        if (dup2(fileno(stderr), fileno(stdout)) < 0) {
            auto error = errno;
            close(saved_);
            throw std::system_error(error, std::generic_category(), "redirecting stdout");
        }
        previous_ = std::cout.rdbuf(std::cerr.rdbuf());
    }

    ~RedirectOutput() {
        std::cout.flush();
        std::fflush(stdout);
        dup2(saved_, fileno(stdout));
        close(saved_);
        std::cout.rdbuf(previous_);
    }

private:
#ifdef _WIN32
    static int dup(int fd) { return _dup(fd); }
    static int dup2(int from, int to) { return _dup2(from, to); }
    static int close(int fd) { return _close(fd); }
    static int fileno(FILE* file) { return _fileno(file); }
#endif
    int saved_;
    std::streambuf* previous_;
};

struct Options {
    std::string input;
    std::string config;
    bool optimize = false;
};

Diagnostics compile(const Options& options, const fs::path& config, lsp::PositionEncodingKind encoding) {
    auto diagnostics = Diagnostics();
    auto fallback    = uri(options.input);
    auto output      = RedirectOutput();
    auto mode        = fe::term::ScopedMode(fe::term::Mode::Never);
    auto driver      = Driver();
    driver.diag(std::make_unique<DiagnosticSink>(driver, diagnostics, fallback, encoding));
    driver.log().set(&std::cerr);
    auto report_error = [&](std::string message) {
        auto& diagnostic    = diagnostics[fallback.toString()].emplace_back();
        diagnostic.message  = std::move(message);
        diagnostic.severity = lsp::DiagnosticSeverity::Error;
        diagnostic.source   = "mim";
    };

    try {
        auto manifest = Manifest();
        if (!config.empty()) {
            fallback = uri(config);
            manifest = load_manifest(config);
            fallback = uri(options.input);
        }

        for (auto&& path : paths_from_env(Plugin_Path_Env))
            driver.add_plugin_path(path);
        for (auto&& path : manifest.plugin_paths)
            driver.add_plugin_path(path);
        for (auto&& path : paths_from_env(Import_Path_Env))
            driver.add_import_path(path);
        for (auto&& path : manifest.import_paths)
            driver.add_import_path(path);
        for (auto&& path : paths_from_env(Prefix_Path_Env))
            driver.add_prefix_path(path);
        for (auto&& path : manifest.prefix_paths)
            driver.add_prefix_path(path);
        for (auto&& arg : manifest.plugin_args) {
            auto pos = arg.find(':');
            if (pos == std::string::npos)
                fe::throwf<std::invalid_argument>("`--plugin-arg` expects `<plugin>:<arg>`, got `{}`", arg);
            driver.add_arg(std::string_view(arg).substr(0, pos), arg.substr(pos + 1));
        }

        auto& world = driver.world();
        world.set(fs::path(options.input).stem().string());
        auto ast    = ast::AST(world);
        auto parser = ast::Parser(ast);
        auto file   = parser.import_main(options.input, manifest.plugins, nullptr);
        if (!file) {
            ast.error().ack();
            fe::throwf("could not read file `{}`", options.input);
        }
        file->bind(ast);
        ast.error().ack();
        file->compile(ast);
        if (options.optimize)
            optimize(world);
        else
            driver.move_to_library(world);
        driver.error().ack();
    } catch (const Error::Bail&) {
    } catch (const std::exception& error) { report_error(error.what()); } catch (...) {
        report_error("unknown compiler exception");
    }
    driver.error().report();
    return diagnostics;
}

class Server {
public:
    explicit Server(Options options)
        : options_(std::move(options))
        , endpoint_(lsp::io::standardIO()) {
        endpoint_.onInitialize([this](const lsp::InitializeParams& params) -> lsp::InitializeResult {
            if (!params.rootUri.isNull() && params.rootUri->isFileUri())
                root_ = params.rootUri->fsPath();
            else if (params.rootPath && !params.rootPath->isNull() && !params.rootPath->value().empty())
                root_ = params.rootPath->value();
            else if (params.workspaceFolders && !params.workspaceFolders->isNull()
                     && !params.workspaceFolders->value().empty()) {
                const auto& folder = params.workspaceFolders->value().front();
                if (folder.uri.isFileUri()) root_ = folder.uri.fsPath();
            }
            position_encoding_ = lsp::PositionEncodingKind::UTF16;
            if (params.capabilities.general && params.capabilities.general->positionEncodings)
                for (const auto& encoding : *params.capabilities.general->positionEncodings)
                    if (encoding == lsp::PositionEncodingKind::UTF8) {
                        position_encoding_ = lsp::PositionEncodingKind::UTF8;
                        break;
                    }
            return {
                .capabilities = {
                    .positionEncoding = position_encoding_,
                    .textDocumentSync = lsp::TextDocumentSyncOptions{
                        .openClose = true,
                        .change = lsp::TextDocumentSyncKind::Full,
                        .save = true,
                    },
                },
                .serverInfo = lsp::ServerInfo{.name = "mim-lsp", .version = MIM_LSP_VERSION},
            };
        });
        endpoint_.onShutdown([]() -> lsp::ShutdownResult { return {}; });
        endpoint_.onExit([] {});
        endpoint_.onInitialized([this](const lsp::InitializedParams&) { recompile(); });
        endpoint_.onTextDocumentDidOpen([this](const lsp::DidOpenTextDocumentParams& params) {
            const auto& document = params.textDocument.uri;
            if (options_.input.empty() && document.isFileUri()) {
                auto path = fs::path(document.fsPath());
                if (path.extension() == ".mim") {
                    options_.input = fs::absolute(path).lexically_normal().string();
                    recompile();
                }
            }
        });
        endpoint_.onTextDocumentDidSave([this](const lsp::DidSaveTextDocumentParams&) { recompile(); });
        endpoint_.onTextDocumentDidChange([this](const lsp::DidChangeTextDocumentParams&) { publish({}); });
        endpoint_.onWorkspaceDidChangeWatchedFiles([this](const lsp::DidChangeWatchedFilesParams& params) {
            for (const auto& change : params.changes)
                if (change.uri.isFileUri() && fs::path(change.uri.fsPath()).filename() == "mim.toml") {
                    recompile();
                    break;
                }
        });
    }

    void run() { endpoint_.runMessageLoop(); }

private:
    void recompile() {
        if (options_.input.empty()) return;
        auto config = fs::path(options_.config);
        if (config.empty() && !root_.empty()) {
            auto candidate = root_ / "mim.toml";
            auto ec        = std::error_code();
            if (fs::exists(candidate, ec) || ec) config = std::move(candidate);
        }
        publish(compile(options_, config, position_encoding_));
    }

    void publish(Diagnostics diagnostics) {
        for (const auto& file : published_)
            if (!diagnostics.contains(file))
                endpoint_.textDocumentPublishDiagnostics({.uri = lsp::Uri::parse(file), .diagnostics = {}});
        published_.clear();
        for (auto& [file, messages] : diagnostics) {
            endpoint_.textDocumentPublishDiagnostics(
                {.uri = lsp::Uri::parse(file), .diagnostics = std::move(messages)});
            published_.insert(file);
        }
    }

    Options options_;
    fs::path root_;
    fe::StrSet published_;
    lsp::PositionEncodingKind position_encoding_ = lsp::PositionEncodingKind::UTF16;
    lsp::ServerEndpoint endpoint_;
};

} // namespace
} // namespace mim

int main(int argc, char** argv) {
    try {
        auto options = mim::Options();
        bool help = false, version = false;
        auto cli = fe::Cli("mim-lsp", "MimIR language server over stdio.")
                       .arg(options.input, "file", "Optional pinned Mim entry-point file.")
                       .opt(options.config, "path", "", "--config", "Reads project configuration from <path>.")
                       .opt(options.optimize, "", "--optimize", "Runs the optimization pipeline and backends.")
                       .help(help)
                       .opt(version, "-v", "--version", "Displays version info and exits.");
        if (auto error = cli.parse(argc, argv)) fe::throwf<std::invalid_argument>("{}", *error);
        if (help) {
            cli.help(std::cout);
            return EXIT_SUCCESS;
        }
        if (version) {
            std::cout << "mim-lsp " << MIM_LSP_VERSION << '\n';
            return EXIT_SUCCESS;
        }
        if (!options.input.empty()) options.input = std::filesystem::absolute(options.input).string();
        if (!options.config.empty()) options.config = std::filesystem::absolute(options.config).string();
        auto server = mim::Server(std::move(options));
        server.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "mim-lsp: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
