#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <windowsx.h>
#include <tchar.h>

#include <limits>
#include <array>
#include <string>
#include <vector>
#include <iostream>
#include <memory>

#include <filesystem>
namespace fs = std::filesystem;

#include <mutex>

#include <wrl.h> 

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;
typedef wchar_t wchar;

#define ASSERT(cond) \
	(void)((!!(cond)) || (std::cout << "Assertion '" << #cond "' failed [" __FILE__ " : " << __LINE__ << "].\n", ::__debugbreak(), 0))

template <typename T> using ref = std::shared_ptr<T>;
template <typename T> using weakref = std::weak_ptr<T>;

template <typename T, typename... Args>
inline ref<T> make_ref(Args&&... args) 
{ 
	return std::make_shared<T>(std::forward<Args>(args)...); 
}

template <typename T> struct is_ref : std::false_type {};
template <typename T> struct is_ref<ref<T>> : std::true_type {};

template <typename T> inline constexpr bool is_ref_v = is_ref<T>::value;


template <typename T>
using com = Microsoft::WRL::ComPtr<T>;

#define arraysize(arr) (sizeof(arr) / sizeof((arr)[0]))


template <typename T>
constexpr inline auto min(T a, T b)
{
	return (a < b) ? a : b;
}

template <typename T>
constexpr inline auto max(T a, T b)
{
	return (a < b) ? b : a;
}

template <auto V> static constexpr auto force_consteval = V;

#define setBit(mask, bit) (mask) |= (1 << (bit))
#define unsetBit(mask, bit) (mask) ^= (1 << (bit))

static void checkResult(HRESULT hr)
{
	ASSERT(SUCCEEDED(hr));
}




