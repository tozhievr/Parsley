module;
#include <string_view>

export module parsley.version;

export namespace parsley {

inline constexpr int VERSION_MAJOR = 0;
inline constexpr int VERSION_MINOR = 1;
inline constexpr int VERSION_PATCH = 0;
inline constexpr std::string_view VERSION_STRING = "0.1.0";

} // namespace parsley
