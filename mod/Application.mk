APP_ABI := arm64-v8a
APP_PLATFORM := android-21
# c++_static on purpose: the game's lib folder has NO libc++_shared.so (checked in lib.zip), and its
# NEEDED list only has libstdc++.so. ProMenu (working plugin) uses c++_static as well.
APP_STL := c++_static
APP_OPTIM := release
APP_CPPFLAGS += -std=c++17
