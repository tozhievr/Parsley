module;
#include <string_view>

export module parsley.version;

export namespace parsley {

inline constexpr int VERSION_MAJOR = 0;
inline constexpr int VERSION_MINOR = 7;
inline constexpr int VERSION_PATCH = 0;
inline constexpr std::string_view VERSION_STRING = "0.7.0";

} // namespace parsley
