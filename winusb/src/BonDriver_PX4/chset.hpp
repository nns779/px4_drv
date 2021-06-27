// chset.hpp

#pragma once

#include <string>
#include <shared_mutex>
#include <vector>
#include <unordered_map>
#include <stdexcept>

#include "type.hpp"

namespace px4 {

class ChannelSet final {
public:
	struct ChannelInfo final {
		ChannelInfo() noexcept : ptx_ch(0), tsid(0) {}
		std::wstring name;
		std::uint32_t ptx_ch;
		std::uint16_t tsid;
	};
	struct SpaceInfo final {
		explicit SpaceInfo(px4::SystemType system = px4::SystemType::UNSPECIFIED) noexcept : SpaceInfo(L"", system) {}
		explicit SpaceInfo(const std::wstring &name, px4::SystemType system = px4::SystemType::UNSPECIFIED) noexcept
			: name(name),
			system(system)
		{}
		std::wstring name;
		px4::SystemType system;
		std::unordered_map<std::uint32_t, ChannelInfo> channels;
	};

public:
	explicit ChannelSet(bool strict = false) noexcept : next_space_id_(0) { SetStrict(strict); }
	~ChannelSet() {}

	void SetStrict(bool strict) noexcept { strict_ = strict; }
	void Clear() noexcept { spaces_.clear(); next_space_id_ = 0; }
	bool Load(const std::wstring &path, px4::SystemType system) noexcept;
	bool Merge(ChannelSet &chset) noexcept;
	bool ExistsSpace(std::uint32_t space_id) const noexcept;
	const std::wstring& GetSpaceName(std::uint32_t space_id) const;
	px4::SystemType GetSpaceSystem(std::uint32_t space_id) const;
	bool ExistsChannel(std::uint32_t space_id, std::uint32_t ch_id) const noexcept;
	const ChannelInfo& GetChannel(std::uint32_t space_id, std::uint32_t ch_id) const;

private:
	bool AddSpace(uint32_t space_id, const std::wstring *name, px4::SystemType system = px4::SystemType::UNSPECIFIED) noexcept;
	bool AddChannel(uint32_t space_id, uint32_t ch_id, const ChannelInfo& ch) noexcept;
	static void Split(char **head, char **tail, char c) noexcept;

	bool strict_;
	mutable std::shared_mutex mtx_;
	std::uint32_t next_space_id_;
	std::vector<SpaceInfo> spaces_;
};

} // namespace px4
