// type.hpp

#pragma once

#include <cstdint>
#include <functional>

#include <guiddef.h>

namespace px4 {

enum class SystemType : std::uint32_t {
	UNSPECIFIED = 0x00,
	ISDB_T = 0x10,
	ISDB_S = 0x20,
};

constexpr SystemType operator&(SystemType left, SystemType right) noexcept
{
	return static_cast<SystemType>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
}

constexpr SystemType operator|(SystemType left, SystemType right) noexcept
{
	return static_cast<SystemType>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr SystemType& operator&=(SystemType& left, SystemType right) noexcept
{
	left = static_cast<SystemType>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
	return left;
}

constexpr SystemType& operator|=(SystemType& left, SystemType right) noexcept
{
	left = static_cast<SystemType>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	return left;
}

} // namespace px4

static inline bool operator<(const GUID &left, const GUID &right) noexcept
{
	return memcmp(&left, &right, sizeof(GUID)) < 0;
}

namespace std {

template<> struct hash<GUID> {
	std::size_t operator()(const GUID &guid) const noexcept
	{
		std::hash<std::size_t> hash;
		const std::size_t *p = reinterpret_cast<const std::size_t *>(&guid);
		std::size_t val = 0;

		for (int i = 0; i < (sizeof(guid) / sizeof(std::size_t)); i++)
			val ^= hash(p[i]);
		
		return val;
	}
};

} // namespace std
