"""PlatformIO extra_script to force a modern C++ standard.

Arduino-ESP32's build scripts often append `-std=gnu++11` late in the flag
list. Some third-party libs (RLottie) require C++14+ (generic lambdas,
std::make_unique, std::enable_if_t).

This script removes any existing `-std=...` from CXXFLAGS/CCFLAGS and appends
`-std=gnu++17` at the end so it wins.
"""

Import("env")  # type: ignore


def _strip_std_flags(flags):
    return [f for f in flags if not (isinstance(f, str) and f.startswith("-std="))]


cxxflags = env.get("CXXFLAGS", [])
ccflags = env.get("CCFLAGS", [])
cflags = env.get("CFLAGS", [])

env.Replace(
    CXXFLAGS=_strip_std_flags(cxxflags) + ["-std=gnu++17"],
    CCFLAGS=_strip_std_flags(ccflags),
    CFLAGS=_strip_std_flags(cflags),
)

print("PlatformIO: forcing -std=gnu++17 (RLottie needs C++14+)")
