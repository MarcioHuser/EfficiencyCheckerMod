#pragma once

template <typename T>
inline const T& min(const T& a, const T& b)
{
    return a < b ? a : b;
}

template <typename T>
inline const T& max(const T& a, const T& b)
{
    return a > b ? a : b;
}
