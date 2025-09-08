#pragma once
/**
 * @file parameters_manager.hpp
 * @brief Public API for a process-local, thread-safe YAML configuration/parameters manager.
 * @details
 * This header exposes a minimal façade to access configuration values anywhere in the
 * process after a one-time initialization. It intentionally provides:
 * - **Strongly-typed getters** so call sites remain explicit and self-documenting.
 * - **Dotted key-path access** (with sequence indexing) to avoid passing YAML nodes around.
 * - **Atomic hot-reload** so long-running services can apply config changes without restarts.
 * - **Shared read / exclusive write locking** (via `std::shared_mutex`) to scale many readers.
 * - **Clear, rich errors** with key path and (when available) YAML source line/column to help
 *   operators quickly locate bad values.
 *
 * Why these affordances are required:
 * - Large C++ services often suffer from "configuration drift" where different modules parse
 *   the same file in different ways. Centralizing access eliminates inconsistencies and makes
 *   validation and observability possible.
 * - Hot-reload is critical in SRE/DevOps workflows (feature-flag flips, tuning parameters) and
 *   must be atomic to avoid torn reads.
 * - Using a singleton/Meyers-instance avoids static-initialization-order fiascos (SIOF) while
 *   still offering global availability.
 */

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace Parameters {

//============================== Exceptions ==============================//

/**
 * @enum ErrorKind
 * @brief High-level class of configuration errors.
 * @details
 * Categorizing errors lets callers decide whether to retry (`ParseError` during reload),
 * fail fast (`ValidationError` at startup), or fallback (`MissingKey` for optional fields).
 * It also improves log clarity and metrics/alerts.
 */
enum class ErrorKind
{
    NotInitialized,  ///< Getters used before init(); prevents SIOF and undefined state.
    MissingKey,      ///< Key path not present in the YAML tree.
    TypeMismatch,    ///< Present but cannot be converted to requested type.
    BadConversion,   ///< Reserved (not used separately) for fine-grained conversions.
    ParseError,      ///< YAML parse error; typically indicates malformed file.
    ValidationError, ///< Validation hook rejected content.
    ReloadError,     ///< Reserved for reload pipeline failures.
    InternalError    ///< Logic/path errors; indicates a bug or malformed key path.
};

/**
 * @class Error
 * @brief Exception type carrying rich diagnostic context.
 * @details
 * We propagate the failing **key path**, **file**, and **line/column** (when available
 * from yaml-cpp) so on-call engineers can pinpoint the exact offending value quickly.
 * The `kind` enables programmatic handling (e.g., convert to HTTP 500 vs 422, metrics, etc.).
 */
class Error : public std::runtime_error {
public:
    ErrorKind kind;      ///< High-level error category.
    std::string keyPath; ///< Dotted key path that failed (may be empty).
    std::string file;    ///< Source file path if known.
    int line = -1;       ///< 0-based line from YAML::Mark, or -1 if unknown.
    int column = -1;     ///< 0-based column from YAML::Mark, or -1 if unknown.

    Error(ErrorKind k, std::string msg, std::string key = {}, std::string src = {},
          int ln = -1, int col = -1)
        : std::runtime_error(std::move(msg)), kind(k), keyPath(std::move(key)),
          file(std::move(src)), line(ln), column(col) {}
};

//============================== Public API ==============================//

/**
 * @brief Initialize the parameters manager with a YAML file path.
 * @param path Absolute or relative path to the main YAML file to load.
 * @throws Parameters::Error on parse/validation failures.
 * @details
 * Loads `defaults.yaml` (if present next to the provided file), then overlays the main file,
 * expands environment variables (`${VAR}` / `${VAR:-default}`), runs validation, and publishes
 * the tree atomically. Using a single entry-point ensures a **single source of truth** and
 * avoids each caller re-parsing in subtly different ways.
 */
void init(const std::string& path);

/**
 * @brief Atomically reload configuration from disk.
 * @throws Parameters::Error when the new configuration fails to parse or validate; in that case,
 *         the previous good configuration remains in effect.
 * @details
 * Performs the same pipeline as `init()` (defaults overlay → env expansion → validation) and
 * swaps the in-memory tree under an exclusive lock. Readers remain consistent (no torn reads)
 * because they acquire a shared lock *after* the swap.
 */
void reload();

/**
 * @brief Check whether a key path exists.
 * @param keyPath Dotted path with optional sequence indices (e.g., `servers[0].port`).
 * @return true if the key exists; false otherwise.
 * @details Useful for optional config probes without incurring exceptions; keeps call sites
 * tidy while still using a centrally-validated tree.
 */
bool has(std::string_view keyPath);

/**
 * @brief Retrieve a value converted to type `T`.
 * @tparam T Destination type (must be supported by yaml-cpp conversions or a `YAML::convert<T>` specialization).
 * @param keyPath Dotted path to the value.
 * @return Parsed value converted to `T`.
 * @throws Parameters::Error with `NotInitialized`, `MissingKey`, or `TypeMismatch`.
 * @details Uses `yaml-cpp`'s `Node::as<T>()` which supports STL types and custom conversions.
 */
template<typename T>
inline T get(std::string_view keyPath);

/**
 * @brief Retrieve an optional value of type `T`.
 * @tparam T Destination type.
 * @param keyPath Dotted path to the value.
 * @return `std::nullopt` if missing; otherwise the converted value.
 * @throws Parameters::Error (`TypeMismatch`) if present but cannot convert to `T`.
 * @details Models optionality explicitly: absence is not exceptional, but bad data still is.
 */
template<typename T>
inline std::optional<T> get_opt(std::string_view keyPath);

/**
 * @brief Retrieve `T` or return a default.
 * @tparam T Destination type.
 * @param keyPath Dotted path to the value.
 * @param defaultVal Value to return if the key is absent.
 * @return Converted value or `defaultVal` when absent.
 * @throws Parameters::Error (`TypeMismatch`) if present but cannot convert to `T`.
 * @details Ideal for feature flags or gradual rollouts with a sane fallback value.
 */
template<typename T>
T get_or(std::string_view keyPath, T defaultVal);

#ifdef PARAMETERS_ENABLE_SET
/**
 * @brief Test-only setter to override configuration at a specific key path.
 * @param keyPath Dotted path to set/override (tests only).
 * @param value New YAML node to assign.
 * @throws Parameters::Error (`NotInitialized`, `ValidationError`).
 * @details Mutations are performed on a deep-cloned copy and re-validated before publish, ensuring
 * that tests cannot accidentally leave the system in an invalid state. Guarded by
 * `PARAMETERS_ENABLE_SET` to prevent accidental use in production.
 */
void set(std::string_view keyPath, const YAML::Node& value);
#endif

/**
 * @brief Validation hook (exposed for reuse in tests/tools).
 * @param root Fully expanded/merged YAML tree to validate.
 * @throws Parameters::Error (`ValidationError`) on any invariant breach.
 * @details Centralizes business constraints near the configuration schema and ensures the same
 * invariants are enforced at `init()` and `reload()` time.
 */
void validate(const YAML::Node& root);

//=========================== Internal bridge ============================//
namespace detail {

/**
 * @brief Resolve a YAML node by dotted key path (throws on absence).
 * @param keyPath Dotted path to resolve.
 * @return Resolved node (defined).
 * @throws Parameters::Error (`NotInitialized`, `MissingKey`).
 * @details Separating node traversal from typed conversion yields clearer error reporting and keeps
 * the public templates small. Internal so we can evolve path semantics without breaking ABI.
 */
YAML::Node get_node(std::string_view keyPath);

/**
 * @brief Resolve a YAML node by dotted key path (returns false on absence).
 * @param keyPath Dotted path to resolve.
 * @param[out] out Set to the resolved node when present; left undefined when absent.
 * @return true if present; false otherwise.
 * @details Used by `get_opt`/`get_or` to avoid throwing for missing keys while sharing traversal.
 */
bool try_get_node(std::string_view keyPath, YAML::Node& out);

#ifdef PARAMETERS_ENABLE_SET
/**
 * @brief Internal helper for test-only key-path assignment (see public `set`).
 * @param keyPath Dotted path to set/override.
 * @param value New YAML node to assign.
 */
void set_node(std::string_view keyPath, const YAML::Node& value);
#endif

/** @internal */
[[noreturn]] inline void throw_not_initialized()
{
    throw Error(ErrorKind::NotInitialized, "Parameters::init() has not been called yet");
}

} // namespace detail

} // namespace Parameters

//================== yaml-cpp custom conversions ==================//
/**
 * @brief `YAML::convert` specialization for `std::chrono::milliseconds`.
 * @details Accepts strings like `"250ms"`, `"2s"`, `"3m"`, or a bare integer (ms). Encoding
 * emits a readable `"<value>ms"` string, keeping values explicit in YAML and code.
 */
namespace YAML {

template <>
struct convert<std::chrono::milliseconds> {
    /** @brief Encode as a human-friendly string (e.g., "250ms"). */
    static Node encode(const std::chrono::milliseconds& rhs) {
        Node node;
        node = std::to_string(rhs.count()) + "ms";
        return node;
    }
    /**
     * @brief Decode from `ms`, `s`, or `m` suffix (or bare integer, ms).
     * @param node Scalar node holding the duration.
     * @param[out] rhs Resulting duration in milliseconds.
     * @return true on success; false on format mismatch.
     */
    static bool decode(const Node& node, std::chrono::milliseconds& rhs) {
        if (!node.IsScalar()) return false;
        const std::string s = node.as<std::string>();
        // Accept integers (assume ms) or "<int><unit>" with unit in {ms,s,m}
        std::size_t i = 0;
        bool neg = false;
        long long val = 0;
        if (i < s.size() && (s[i] == '+' || s[i] == '-'))
        {
            neg = (s[i] == '-');
            ++i;
        }
        for (; i < s.size(); ++i)
        {
            if (std::isdigit(static_cast<unsigned char>(s[i])) <= 0)
            {
                break;
            }

            val = (val * 10) + (s[i] - '0');
        }
        const std::string unit = s.substr(i);
        long long ms = 0;
        if (unit.empty() || unit == "ms") ms = val;
        else if (unit == "s") ms = val * 1000LL;
        else if (unit == "m") ms = val * 60LL * 1000LL;
        else return false;
        rhs = std::chrono::milliseconds(neg ? -ms : ms);
        return true;
    }
};

} // namespace YAML

//===================== Template definitions =====================//
namespace Parameters
{

template<typename T>
inline T get(std::string_view keyPath)
{
    YAML::Node node = detail::get_node(keyPath);
    try {
        return node.as<T>();
    } catch (const YAML::BadConversion& bc) {
        const auto mark = node.Mark();
        throw Error(ErrorKind::TypeMismatch,
            std::format("Type mismatch for '{}': {}", keyPath, bc.what()),
            std::string(keyPath),
            /*file*/ "",
            mark.line,
            mark.column);
    }
}

template<typename T>
inline std::optional<T> get_opt(std::string_view keyPath)
{
    YAML::Node node;
    if (!detail::try_get_node(keyPath, node)) return std::nullopt;
    try {
        return node.as<T>();
    } catch (const YAML::BadConversion& bc) {
        const auto mark = node.Mark();
        throw Error(ErrorKind::TypeMismatch,
            std::format("Type mismatch for '{}': {}", keyPath, bc.what()),
            std::string(keyPath),
            "",
            mark.line,
            mark.column);
    }
}

template<typename T>
T get_or(std::string_view keyPath, T defaultVal)
{
    if (auto opt = get_opt<T>(keyPath)) return *opt;
    return defaultVal;
}

} // namespace Parameters