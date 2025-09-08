/**
 * @file parameters_manager.cpp
 * @brief Implementation of the process-local, thread-safe YAML parameters manager.
 * @details
 * This translation unit keeps the mutable internals (singleton state, deep-clone buffers,
 * parsing/validation helpers) out of the header to keep compile times small and preserve
 * ABI stability for downstream users.
 */

#include "parameters_manager/parameters_manager.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <regex>
#include <sstream>

#ifndef APP_PROJECT_SOURCE_DIR
#define APP_PROJECT_SOURCE_DIR ""
#endif

namespace fs = std::filesystem;

namespace
{
using namespace Parameters;

//========================= Implementation singleton =========================//

/**
 * @brief Opaque state container for the parameters manager.
 * @details
 * - `root` is held behind a `shared_ptr` so we can atomically swap the entire tree on reload
 *   while readers keep a stable reference during their critical section.
 * - `std::shared_mutex` enables many readers and one writer (reload or test set).
 */
struct Impl
{
    mutable std::shared_mutex mtx;
    std::shared_ptr<YAML::Node> root; ///< Current immutable tree for readers.
    std::string main_path;            ///< Path to the primary YAML file.
    std::string defaults_path;        ///< Optional defaults file path.
    bool initialized = false;         ///< Guards against pre-init usage.

    static Impl& instance()
    {
        static Impl s;
        return s;
    }
};

//------------------------------- Helpers --------------------------------//

/**
 * @brief Deep-clone a YAML node (map/sequence/scalar) to break aliasing.
 * @param n Source node.
 * @return A structurally identical node without shared substructure.
 * @details yaml-cpp nodes are handles; cloning ensures test `set()` and reload build on a copy so
 * readers never observe partially-mutated state.
 */
YAML::Node deep_clone(const YAML::Node& n)
{
    if (!n.IsDefined())
    {
        return YAML::Node {};
    }
    if (n.IsScalar())
    {
        return YAML::Node(n.Scalar());
    }
    if (n.IsSequence())
    {
        YAML::Node out(YAML::NodeType::Sequence);
        for (std::size_t i = 0; i < n.size(); ++i)
        {
            out.push_back(deep_clone(n[i]));
        }
        return out;
    }
    if (n.IsMap())
    {
        YAML::Node out(YAML::NodeType::Map);
        for (auto it : n)
        {
            out[it.first] = deep_clone(it.second);
        }
        return out;
    }
    return YAML::Node {};
}

/**
 * @brief Deep-merge `src` onto `dst` (maps merged; sequences/scalars replaced).
 * @param dst Destination node to be updated in-place.
 * @param src Source node whose values overlay `dst`.
 * @details Implements defaults overlay: `defaults.yaml` first, then main file wins. Replacing
 * sequences wholesale avoids surprising concatenation of lists across layers.
 */
void merge_nodes(YAML::Node& dst, const YAML::Node& src)
{
    if (!src || !src.IsDefined())
    {
        return;
    }

    if (!dst || !dst.IsDefined())
    {
        dst = deep_clone(src);
        return;
    }

    if (src.IsMap() && dst.IsMap())
    {
        for (auto it : src)
        {
            auto key = it.first;
            auto sVal = it.second;
            auto dVal = dst[key];
            if (sVal.IsMap() && dVal.IsMap())
            {
                merge_nodes(dVal, sVal);
            }
            else
            {
                dst[key] = deep_clone(sVal);
            }
        }
    }
    else
    {
        // Sequences and scalars: replace wholesale
        dst = deep_clone(src);
    }
}

/**
 * @brief Expand `${VAR}` and `${VAR:-default}` in a scalar string.
 * @param in Input string possibly containing environment placeholders.
 * @return Expanded string with substitutions applied.
 */
std::string expand_env(const std::string& in)
{
    // Regex for ${VAR} and ${VAR:-default}
    static const std::regex re(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\})");
    std::string out;
    out.reserve(in.size());
    std::sregex_iterator it(in.begin(), in.end(), re), end;
    std::size_t last = 0;
    for (; it != end; ++it)
    {
        const auto& m = *it;
        out.append(in, last, static_cast<std::size_t>(m.position() - last));
        std::string var = m[1].str();
        std::string def = m[2].matched ? m[2].str() : "";
        const char* val = std::getenv(var.c_str());
        out += (val && *val) ? std::string(val) : def;
        last = static_cast<std::size_t>(m.position() + m.length());
    }
    out.append(in, last, std::string::npos);
    return out;
}

/**
 * @brief Apply environment expansion recursively to a YAML node.
 * @param node Root node to mutate in-place.
 * @details Done in-place on a cloned tree prior to validation so validators see resolved values.
 */
void expand_env_inplace(YAML::Node node)
{
    if (!node.IsDefined())
    {
        return;
    }
    if (node.IsScalar())
    {
        auto s = node.as<std::string>();
        auto t = expand_env(s);
        if (t != s)
        {
            node = t;
        }
        return;
    }
    if (node.IsSequence())
    {
        for (std::size_t i = 0; i < node.size(); ++i)
        {
            expand_env_inplace(node[i]);
        }
        return;
    }
    if (node.IsMap())
    {
        for (auto it : node)
        {
            expand_env_inplace(it.second);
        }
    }
}

/**
 * @brief One segment of a dotted key path (with optional indices).
 */
struct Segment
{
    std::string key;
    std::queue<std::size_t> idx;
};

/**
 * @brief Parse a dotted key path into segments.
 * @details
 * This function takes a dotted key path (e.g., "a.b[0].c") and splits it into a vector of `Segment` objects.
 * Each `Segment` represents a part of the path, including keys and optional sequence indices.
 *
 * This is required to enable traversal of deeply nested YAML structures, where keys and indices
 * are used to locate specific nodes. For example:
 * - "a.b[0].c" would be parsed into segments:
 *   - Segment with key "a"
 *   - Segment with key "b"
 *   - Segment with index `[0]`
 *   - Segment with key "c"
 *
 * This parsing allows the system to handle complex paths in a structured way, enabling
 * operations like retrieval, validation, and mutation of YAML nodes.
 *
 * @param path The dotted key path to parse (e.g., "a.b[0].c").
 * @return A vector of `Segment` objects representing the parsed path.
 * @throws Parameters::Error if the key path is malformed (e.g., missing closing brackets).
 */
std::queue<Segment> parse_keypath(std::string_view path)
{
    std::queue<Segment> segs;
    // Insert empty segment representing root node
    (void)segs.emplace();

    Segment cur;
    for (std::size_t i = 0; i < path.size();)
    {
        char c = path[i];
        if (c == '.')
        {
            segs.push(cur);
            cur = Segment {};
            ++i;
            continue;
        }
        else if (c == '[')
        {
            ++i;
            std::size_t v = 0;
            bool got = false;
            for (; i < path.size(); ++i)
            {
                if (std::isdigit(static_cast<unsigned char>(path[i])) <= 0)
                {
                    break;
                }
                v = (v * 10) + (path[i] - '0');
                got = true;
            }
            if (!got || i >= path.size() || path[i] != ']')
                throw Error(ErrorKind::InternalError, "Malformed keyPath: missing ']' ");
            cur.idx.push(v);
            ++i;
            continue;
        }
        else
        {
            cur.key.push_back(c);
            ++i;
        }
    }
    segs.push(cur);
    return segs;
}
/**
 * @brief Traverse the YAML tree by key path and return the found node.
 * @param n        Current YAML node to inspect.
 * @param segs     Queue of path segments. Caller is expected to seed it with an
 *                 empty "root" segment first, followed by actual path segments.
 * @param existedOut  Set to true when a node is successfully resolved; false otherwise.
 * @param markOut     Set to the resolved node's Mark() when found (for error reporting).
 * @return Resolved node, or an undefined node on failure.
 *
 * Notes:
 * - Keeps the original behavior: scalars are considered terminal (success) even
 *   if additional segments remain.
 * - Adds defensive checks to avoid undefined behavior when the queue underflows.
 * - No loops used; recursion is preserved.
 */
YAML::Node traverse(const YAML::Node& n, std::queue<Segment>& segs, bool* existedOut, YAML::Mark* markOut)
{
    // Small helpers to set outputs consistently.
    auto fail = [&]() -> YAML::Node {
        if (existedOut)
            *existedOut = false;
        return {};
    };
    auto succeed = [&](const YAML::Node& here) -> YAML::Node {
        if (existedOut)
            *existedOut = true;
        if (markOut)
            *markOut = here.Mark();
        return here;
    };

    // Node must be valid/defined.
    if (!n || !n.IsDefined())
        return fail();

    // Original logic: encountering a scalar is terminal success.
    if (n.IsScalar())
        return succeed(n);

    // If segs is empty here, we can't advance the path. Treat as not found.
    // (With the root placeholder convention, this should rarely happen.)
    if (segs.empty())
        return fail();

    if (n.IsMap())
    {
        // Advance one segment. On the first call this discards the root placeholder.
        segs.pop();

        // Path fully consumed -> return the current (map) node.
        // This prevents segs.front() UB and matches the intended resolution semantics.
        if (segs.empty())
            return succeed(n);

        const Segment& s = segs.front();

        // Defensive: empty key cannot be resolved in a map.
        if (s.key.empty())
            return fail();

        // Descend via key. If missing, YAML returns an undefined node; recursion will fail().
        return traverse(n[s.key], segs, existedOut, markOut);
    }

    if (n.IsSequence())
    {
        // Stay on the same Segment while consuming sequence indices from its idx queue.
        Segment& s = segs.front();
        auto& idx = s.idx;

        // Original behavior: an index is required to traverse sequences.
        if (idx.empty())
            return fail();

        const std::size_t i = idx.front();
        idx.pop();

        // Bounds check for the sequence.
        if (i >= n.size())
            return fail();

        // Descend into the i-th element.
        return traverse(n[i], segs, existedOut, markOut);
    }

    // Neither scalar/map/sequence (e.g., Null) -> not found.
    return fail();
}

//--------------------------- Build pipeline ---------------------------//

/**
 * @brief Load, overlay defaults, expand env, validate, and return final tree.
 * @param mainPath Path to the main YAML file.
 * @param[out] defaultsOut Set to the path of defaults file if used; empty otherwise.
 * @return Final validated YAML tree.
 * @details Centralizes the end-to-end pipeline so both `init()` and `reload()` use identical logic.
 */
YAML::Node load_and_build(const std::string& path, std::string& defaultsOut)
{
    YAML::Node base;

    fs::path mainPath = fs::path(APP_PROJECT_SOURCE_DIR) / path;

    try
    {
        // Optional defaults.yaml alongside main
        auto dflt = mainPath.parent_path() / "defaults.yaml";
        if (fs::exists(dflt))
        {
            defaultsOut = dflt.string();
            base = YAML::LoadFile(dflt.string());
        }
        YAML::Node overlay = YAML::LoadFile(mainPath.string());
        if (base)
        {
            merge_nodes(base, overlay);
        }
        else
        {
            base = overlay;
        }

        // Expand ${ENV} in-place
        expand_env_inplace(base);
        Parameters::validate(base);
        return base;
    } catch (const YAML::ParserException& e)
    {
        throw Parameters::Error(Parameters::ErrorKind::ParseError,
            std::string("YAML parse error: ") + e.what(),
            "",
            mainPath.string(),
            e.mark.line,
            e.mark.column);
    } catch (const Parameters::Error&)
    {
        throw;
    } catch (const std::exception& e)
    {
        throw Parameters::Error(
            Parameters::ErrorKind::ParseError, std::string("Failed to load parameters: ") + e.what());
    }
}

} // anonymous namespace

//=============================== Public API ===============================//
namespace Parameters
{

void init(const std::string& path)
{
    Impl& S = Impl::instance();
    std::unique_lock lk(S.mtx);
    std::string defaultsUsed;
    YAML::Node built = load_and_build(path, defaultsUsed); // defined below via ADL
    S.root = std::make_shared<YAML::Node>(std::move(built));
    S.main_path = path;
    S.defaults_path = defaultsUsed;
    S.initialized = true;
}

void reload()
{
    Impl& S = Impl::instance();
    std::unique_lock lk(S.mtx);
    if (!S.initialized)
    {
        detail::throw_not_initialized();
    }
    std::string defaultsUsed;
    YAML::Node built = load_and_build(S.main_path, defaultsUsed);
    S.root = std::make_shared<YAML::Node>(std::move(built));
    S.defaults_path = defaultsUsed;
}

bool has(std::string_view keyPath)
{
    Impl& S = Impl::instance();
    std::shared_lock lk(S.mtx);
    if (!S.initialized)
    {
        detail::throw_not_initialized();
    }
    bool existed = false;
    auto segs = parse_keypath(keyPath);
    (void)traverse(*S.root, segs, &existed, nullptr);
    return existed;
}

// Exposed to Parameters namespace for reuse in tests/tools
void validate(const YAML::Node& root)
{
    auto require = [](std::string_view path, const YAML::Node& n) {
        if (!n || !n.IsDefined())
        {
            throw Error(ErrorKind::ValidationError,
                std::string("Missing required key: ") + std::string(path),
                std::string(path));
        }
    };

    // Required: db.primary.port in [1,65535]
    YAML::Node port = root["db"]["primary"]["port"];
    require("db.primary.port", port);
    if (!port.IsScalar())
    {
        throw Error(ErrorKind::ValidationError, "db.primary.port must be a scalar integer", "db.primary.port");
    }

    auto pval = port.as<long long>();
    if (pval < 1 || pval > 65535)
    {
        throw Error(ErrorKind::ValidationError,
            "db.primary.port must be in [1,65535], got " + std::to_string(pval),
            "db.primary.port",
            /*file*/ "",
            port.Mark().line,
            port.Mark().column);
    }

    // servers non-empty
    YAML::Node servers = root["servers"];
    require("servers", servers);
    if (!servers.IsSequence() || servers.size() == 0)
    {
        throw Error(ErrorKind::ValidationError, "servers must be a non-empty sequence", "servers");
    }

    // pool.maxConnections >= 1
    YAML::Node maxConn = root["db"]["pool"]["maxConnections"];
    require("db.pool.maxConnections", maxConn);
    if (maxConn.as<long long>() < 1)
    {
        throw Error(ErrorKind::ValidationError,
            "db.pool.maxConnections must be >= 1",
            "db.pool.maxConnections",
            "",
            maxConn.Mark().line,
            maxConn.Mark().column);
    }
}

} // namespace Parameters

//--------------------------- Public bridge ---------------------------//
namespace Parameters::detail
{

YAML::Node get_node(std::string_view keyPath)
{
    const Impl& S = Impl::instance();
    std::shared_lock lk(S.mtx);
    if (!S.initialized)
    {
        throw_not_initialized();
    }

    bool existed = false;
    YAML::Mark mark;
    auto segs = parse_keypath(keyPath);
    YAML::Node node = traverse(*S.root, segs, &existed, &mark);
    if (!existed)
    {
        throw Error(ErrorKind::MissingKey,
            std::string("Missing key '") + std::string(keyPath) + "'",
            std::string(keyPath),
            /*file*/ "",
            mark.line,
            mark.column);
    }
    return node; // copy (handle) is cheap
}

bool try_get_node(std::string_view keyPath, YAML::Node& out)
{
    const Impl& S = Impl::instance();
    std::shared_lock lk(S.mtx);
    if (!S.initialized)
        throw_not_initialized();
    bool existed = false;
    auto segs = parse_keypath(keyPath);
    out = traverse(*S.root, segs, &existed, nullptr);
    return existed;
}

#ifdef PARAMETERS_ENABLE_SET

/**
 * @brief Ensure a key path exists (creating maps/sequences as needed) and return the leaf.
 * @param root Root of the tree to mutate.
 * @param segs Parsed key-path segments.
 * @return Mutable leaf node corresponding to the path.
 */
YAML::Node ensure_path_exists(YAML::Node root, const std::queue<Segment>& segs)
{
    YAML::Node node = root;
    for (const auto& s : segs)
    {
        if (!s.key.empty())
        {
            if (!node[s.key] || !node[s.key].IsDefined())
            {
                node[s.key] = YAML::Node(YAML::NodeType::Null);
            }
            node = node[s.key];
        }
        for (auto idx : s.idx)
        {
            if (!node || (!node.IsSequence() && !node.IsNull()))
            {
                node = node = YAML::Node(YAML::NodeType::Sequence); // reset to sequence
            }
            if (node.IsNull())
                node = YAML::Node(YAML::NodeType::Sequence);
            while (node.size() <= idx)
                node.push_back(YAML::Node());
            node = node[idx];
        }
    }
    return node;
}

/**
 * @brief Test-only setter internal implementation.
 * @param keyPath Dotted path to set/override.
 * @param value New YAML node to assign.
 * @details Performs copy-on-write then re-validates before publishing.
 */
void set_node(std::string_view keyPath, const YAML::Node& value)
{
    Impl& S = Impl::instance();
    std::unique_lock lk(S.mtx);
    if (!S.initialized)
        throw_not_initialized();

    // Work on a copy then swap (to keep readers consistent)
    YAML::Node copy = deep_clone(*S.root);
    auto segs = parse_keypath(keyPath);
    // traverse bottom-1, then assign
    if (segs.empty())
        throw Error(ErrorKind::InternalError, "Empty keyPath");

    YAML::Node leaf = ensure_path_exists(copy, segs);
    leaf = value;

    // Validate new tree before publish to protect readers
    validate(copy);
    S.root = std::make_shared<YAML::Node>(std::move(copy));
}

#endif // PARAMETERS_ENABLE_SET

} // namespace Parameters::detail

#ifdef PARAMETERS_ENABLE_SET
namespace Parameters
{
void set(std::string_view keyPath, const YAML::Node& value)
{
    detail::set_node(keyPath, value);
}
} // namespace Parameters
#endif
