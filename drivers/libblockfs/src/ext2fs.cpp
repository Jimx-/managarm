
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iostream>

#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <helix/memory.hpp>

#include "ext2fs.hpp"

namespace blockfs {
namespace ext2fs {

namespace {
	constexpr bool logSuperblock = true;

	constexpr int pageShift = 12;
	constexpr size_t pageSize = size_t{1} << pageShift;
}

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

Inode::Inode(FileSystem &fs, uint32_t number)
: fs(fs), number(number), isReady(false) { }

COFIBER_ROUTINE(async::result<std::experimental::optional<DirEntry>>,
		Inode::findEntry(std::string name), ([=] {
	assert(!name.empty() && name != "." && name != "..");

	COFIBER_AWAIT readyJump.async_wait();

	helix::LockMemory lock_memory;
	auto map_size = (fileSize + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemory(helix::BorrowedDescriptor(frontalMemory), &lock_memory,
			0, map_size, helix::Dispatcher::global());
	COFIBER_AWAIT submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// TODO: Use a RAII mapping class to get rid of the mapping on return.
	// map the page cache into the address space
	void *window;
	HEL_CHECK(helMapMemory(frontalMemory, kHelNullHandle, nullptr, 0, map_size,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking, &window));

	// read the directory structure
	uintptr_t offset = 0;
	while(offset < fileSize) {
		auto disk_entry = reinterpret_cast<DiskDirEntry *>((char *)window + offset);
		// TODO: use memcmp?
		if(name.length() == disk_entry->nameLength
				&& strncmp(disk_entry->name, name.c_str(), disk_entry->nameLength) == 0) {
			DirEntry entry;
			entry.inode = disk_entry->inode;

			switch(disk_entry->fileType) {
			case EXT2_FT_REG_FILE:
				entry.fileType = kTypeRegular; break;
			case EXT2_FT_DIR:
				entry.fileType = kTypeDirectory; break;
			case EXT2_FT_SYMLINK:
				entry.fileType = kTypeSymlink; break;
			default:
				entry.fileType = kTypeNone;
			}

			COFIBER_RETURN(entry);
		}

		offset += disk_entry->recordLength;
	}
	assert(offset == fileSize);

	COFIBER_RETURN(std::experimental::nullopt);
}))

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

FileSystem::FileSystem(BlockDevice *device)
: device(device) {
}

COFIBER_ROUTINE(async::result<void>, FileSystem::init(), ([=] {
	std::vector<uint8_t> buffer(1024);
	COFIBER_AWAIT device->readSectors(2, buffer.data(), 2);

	DiskSuperblock sb;
	memcpy(&sb, buffer.data(), sizeof(DiskSuperblock));
	assert(sb.magic == 0xEF53);

	inodeSize = sb.inodeSize;
	blockShift = 10 + sb.logBlockSize;
	blockSize = 1024 << sb.logBlockSize;
	blockPagesShift = blockShift < pageShift ? pageShift : blockShift;
	sectorsPerBlock = blockSize / 512;
	numBlockGroups = sb.blocksCount / sb.blocksPerGroup;
	inodesPerGroup = sb.inodesPerGroup;

	if(logSuperblock) {
		std::cout << "ext2fs: Block size is: " << blockSize << std::endl;
		std::cout << "ext2fs: Optional features: " << sb.featureCompat
				<< ", w-required features: " << sb.featureRoCompat
				<< ", r/w-required features: " << sb.featureIncompat << std::endl;
	}

	auto bgdt_size = (numBlockGroups * sizeof(DiskGroupDesc) + 511) & ~size_t(511);
	// TODO: Use std::string instead of malloc().
	blockGroupDescriptorBuffer = malloc(bgdt_size);

	auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
	COFIBER_AWAIT device->readSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
			blockGroupDescriptorBuffer, bgdt_size / 512);

	COFIBER_RETURN();
}))

auto FileSystem::accessRoot() -> std::shared_ptr<Inode> {
	return accessInode(EXT2_ROOT_INO);
}

auto FileSystem::accessInode(uint32_t number) -> std::shared_ptr<Inode> {
	assert(number > 0);
	std::weak_ptr<Inode> &inode_slot = activeInodes[number];
	std::shared_ptr<Inode> active_inode = inode_slot.lock();
	if(active_inode)
		return std::move(active_inode);

	auto new_inode = std::make_shared<Inode>(*this, number);
	inode_slot = std::weak_ptr<Inode>(new_inode);
	initiateInode(new_inode);

	return std::move(new_inode);
}

COFIBER_ROUTINE(cofiber::no_future, FileSystem::initiateInode(std::shared_ptr<Inode> inode),
		([=] {
	uint32_t block_group = (inode->number - 1) / inodesPerGroup;
	uint32_t index = (inode->number - 1) % inodesPerGroup;
	uint32_t offset = index * inodeSize;

	auto bgdt = (DiskGroupDesc *)blockGroupDescriptorBuffer;
	uint32_t inode_table_block = bgdt[block_group].inodeTable;

	std::vector<uint8_t> buffer(512);
	uint32_t sector = inode_table_block * sectorsPerBlock + (offset / 512);
	COFIBER_AWAIT device->readSectors(sector, buffer.data(), 1);

	DiskInode disk_inode;
	memcpy(&disk_inode, buffer.data() + (offset % 512), sizeof(DiskInode));
//	printf("Inode %u: file size: %u\n", inode->number, disk_inode.size);

	if((disk_inode.mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
		inode->fileType = kTypeRegular;
	}else if((disk_inode.mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
		inode->fileType = kTypeSymlink;
	}else if((disk_inode.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
		inode->fileType = kTypeDirectory;
	}else{
		std::cerr << "ext2fs: Unexpected inode type " << (disk_inode.mode & EXT2_S_IFMT)
				<< " for inode " << inode->number << std::endl;
		abort();
	}

	// TODO: support large files
	inode->fileSize = disk_inode.size;
	inode->fileData = disk_inode.data;

	// filter out the file type from the mode
	// TODO: ext2fs stores a 32-bit mode
	inode->mode = disk_inode.mode & 0x0FFF;

	inode->numLinks = disk_inode.linksCount;
	// TODO: support large uid / gids
	inode->uid = disk_inode.uid;
	inode->gid = disk_inode.gid;
	inode->accessTime.tv_sec = disk_inode.atime;
	inode->accessTime.tv_nsec = 0;
	inode->dataModifyTime.tv_sec = disk_inode.mtime;
	inode->dataModifyTime.tv_nsec = 0;
	inode->anyChangeTime.tv_sec = disk_inode.ctime;
	inode->anyChangeTime.tv_nsec = 0;

	// Allocate a page cache for the file.
	auto cache_size = (inode->fileSize + 0xFFF) & ~size_t(0xFFF);
	HEL_CHECK(helCreateManagedMemory(cache_size, kHelAllocBacked,
			&inode->backingMemory, &inode->frontalMemory));

	inode->isReady = true;
	inode->readyJump.trigger();

	HelHandle frontalOrder1, frontalOrder2;
	HelHandle backingOrder1, backingOrder2;
	HEL_CHECK(helCreateManagedMemory(3 << blockPagesShift,
			kHelAllocBacked, &backingOrder1, &frontalOrder1));
	HEL_CHECK(helCreateManagedMemory((blockSize / 4) << blockPagesShift,
			kHelAllocBacked, &backingOrder2, &frontalOrder2));
	inode->indirectOrder1 = helix::UniqueDescriptor{frontalOrder1};
	inode->indirectOrder2 = helix::UniqueDescriptor{frontalOrder2};

	manageIndirect(inode, 1, helix::UniqueDescriptor{backingOrder1});
	manageIndirect(inode, 2, helix::UniqueDescriptor{backingOrder2});
	manageFileData(inode);
}))

COFIBER_ROUTINE(cofiber::no_future, FileSystem::manageFileData(std::shared_ptr<Inode> inode),
		([=] {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(helix::BorrowedDescriptor(inode->backingMemory),
				&manage, helix::Dispatcher::global());
		COFIBER_AWAIT(submit.async_wait());
		HEL_CHECK(manage.error());
		assert(manage.offset() + manage.length() <= ((inode->fileSize + 0xFFF) & ~size_t(0xFFF)));

		void *window;
		HEL_CHECK(helMapMemory(inode->backingMemory, kHelNullHandle, nullptr,
				manage.offset(), manage.length(), kHelMapProtRead | kHelMapProtWrite, &window));

		size_t read_size = std::min(manage.length(), inode->fileSize - manage.offset());
		size_t num_blocks = read_size / inode->fs.blockSize;
		if(read_size % inode->fs.blockSize != 0)
			num_blocks++;

		assert(manage.offset() % inode->fs.blockSize == 0);
		assert(num_blocks * inode->fs.blockSize <= manage.length());
		COFIBER_AWAIT inode->fs.readData(inode, manage.offset() / inode->fs.blockSize,
				num_blocks, window);

		HEL_CHECK(helCompleteLoad(inode->backingMemory, manage.offset(), manage.length()));
		HEL_CHECK(helUnmapMemory(kHelNullHandle, window, manage.length()));
	}
}))

COFIBER_ROUTINE(cofiber::no_future, FileSystem::manageIndirect(std::shared_ptr<Inode> inode,
		int order, helix::UniqueDescriptor the_memory), ([=, memory = std::move(the_memory)] {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		COFIBER_AWAIT(submit_manage.async_wait());
		HEL_CHECK(manage.error());

		uint32_t element = manage.offset() >> blockPagesShift;

		uint32_t block;
		if(order == 1) {
			switch(element) {
			case 0: block = inode->fileData.blocks.singleIndirect; break;
			case 1: block = inode->fileData.blocks.doubleIndirect; break;
			case 2: block = inode->fileData.blocks.tripleIndirect; break;
			default:
				assert(!"unexpected offset");
				abort();
			}
		}else{
			assert(order == 2);

			auto indirect_frame = element >> (blockShift - 2);
			auto indirect_index = element & ((1 << (blockShift - 2)) - 1);

			helix::LockMemory lock_indirect;
			auto &&submit_indirect = helix::submitLockMemory(inode->indirectOrder1,
					&lock_indirect,
					(1 + indirect_frame) << blockPagesShift, 1 << blockPagesShift,
					helix::Dispatcher::global());
			COFIBER_AWAIT submit_indirect.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					(1 + indirect_frame) << blockPagesShift, 1 << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};
			block = reinterpret_cast<uint32_t *>(indirect_map.get())[indirect_index];
		}

		assert(!(manage.offset() & ((1 << blockPagesShift) - 1))
				&& "TODO: propery support multi-page blocks");
		assert(manage.length() == (1 << blockPagesShift)
				&& "TODO: propery support multi-page blocks");

		helix::Mapping out_map{memory, manage.offset(), manage.length()};
		COFIBER_AWAIT device->readSectors(block * sectorsPerBlock,
				out_map.get(), sectorsPerBlock);
		HEL_CHECK(helCompleteLoad(memory.getHandle(), manage.offset(), manage.length()));
	}
}))

COFIBER_ROUTINE(async::result<void>, FileSystem::readData(std::shared_ptr<Inode> inode,
		uint64_t offset, size_t num_blocks, void *buffer), ([=] {
	// We perform "read-fusion" here i.e. we try to read multiple
	// consecutive blocks in a single readSectors() operation.
	auto fuse = [] (size_t index, size_t remaining, uint32_t *list, size_t limit) {
		size_t n = 1;
		while(n < remaining && index + n < limit) {
			if(list[index + n] != list[index] + n)
				break;
			n++;
		}
		return std::pair<size_t, size_t>{list[index], n};
	};

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	COFIBER_AWAIT inode->readyJump.async_wait();
	// TODO: Assert that we do not read past the EOF.

	size_t progress = 0;
	while(progress < num_blocks) {
		// Block number and block count of the readSectors() command that we will issue here.
		std::pair<size_t, size_t> issue;

		auto index = offset + progress;
//		std::cout << "Reading " << index << "-th block from inode " << inode->number
//				<< " (" << progress << "/" << num_blocks << " in request)" << std::endl;

		assert(index < d_range);
		if(index >= d_range) {
			assert(!"Fix triple indirect blocks");
		}else if(index >= s_range) { // Use the double indirect block.
			// TODO: Use shift/and instead of div/mod.
			auto indirect_frame = (index - s_range) >> (blockShift - 2);
			auto indirect_index = (index - s_range) & ((1 << (blockShift - 2)) - 1);

			helix::LockMemory lock_indirect;
			auto &&submit = helix::submitLockMemory(inode->indirectOrder2, &lock_indirect,
					indirect_frame << blockPagesShift, 1 << blockPagesShift,
					helix::Dispatcher::global());
			COFIBER_AWAIT submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder2,
					indirect_frame << blockPagesShift, 1 << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};

			issue = fuse(indirect_index, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else if(index >= i_range) { // Use the triple indirect block.
			helix::LockMemory lock_indirect;
			auto &&submit = helix::submitLockMemory(inode->indirectOrder1,
					&lock_indirect, 0, 1 << blockPagesShift,
					helix::Dispatcher::global());
			COFIBER_AWAIT submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					0, 1 << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};
			issue = fuse(index - i_range, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else{
			issue = fuse(index, num_blocks - progress, inode->fileData.blocks.direct, 12);
		}

//		std::cout << "Issuing read of " << issue.second
//				<< " blocks, starting at " << issue.first << std::endl;

		assert(issue.first != 0);
		COFIBER_AWAIT device->readSectors(issue.first * sectorsPerBlock,
				(uint8_t *)buffer + progress * blockSize,
				issue.second * sectorsPerBlock);
		progress += issue.second;
	}

	COFIBER_RETURN();
}))

// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

OpenFile::OpenFile(std::shared_ptr<Inode> inode)
: inode(inode), offset(0) { }

COFIBER_ROUTINE(async::result<std::optional<std::string>>,
OpenFile::readEntries(), ([=] {
	COFIBER_AWAIT inode->readyJump.async_wait();

	assert(offset <= inode->fileSize);
	if(offset == inode->fileSize)
		COFIBER_RETURN(std::nullopt);

	auto map_size = (inode->fileSize + 0xFFF) & ~size_t(0xFFF);

	helix::LockMemory lock_memory;
	auto &&submit = helix::submitLockMemory(helix::BorrowedDescriptor(inode->frontalMemory),
			&lock_memory, 0, map_size, helix::Dispatcher::global());
	COFIBER_AWAIT submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// TODO: Use a RAII mapping class to get rid of the mapping on return.
	// map the page cache into the address space
	void *window;
	HEL_CHECK(helMapMemory(inode->frontalMemory, kHelNullHandle, nullptr, 0, map_size,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking, &window));

	// Read the directory structure.
	auto disk_entry = reinterpret_cast<DiskDirEntry *>((char *)window + offset);
	assert(offset + sizeof(DiskDirEntry) <= inode->fileSize);
	assert(offset + disk_entry->recordLength <= inode->fileSize);
	offset += disk_entry->recordLength;
//	std::cout << "libblockfs: Returning entry "
//			<< std::string(disk_entry->name, disk_entry->nameLength) << std::endl;
	COFIBER_RETURN(std::string(disk_entry->name, disk_entry->nameLength));
}))

} } // namespace blockfs::ext2fs

