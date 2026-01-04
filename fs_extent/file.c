#include <linux/fs.h>
#include <linux/uaccess.h>
#include "osfs.h"

/**
 * Function: osfs_get_phys_block
 * Description: Maps a logical block number to a physical block number using extents.
 * Returns: Physical block number, or 0 if not mapped.
 */
uint32_t osfs_get_phys_block(struct osfs_inode *osfs_inode, uint32_t logical_block)
{
    int i;
    for (i = 0; i < osfs_inode->extent_count; i++) {
        struct osfs_extent *ext = &osfs_inode->extents[i];
        
        if (logical_block >= ext->ee_block && 
            logical_block < (ext->ee_block + ext->ee_len)) {
            uint32_t offset = logical_block - ext->ee_block;
            return ext->ee_start + offset;
        }
    }
    return 0; // Not mapped (Sparse file or End of File)
}

/**
 * Function: osfs_read
 * Description: Reads data from a file.
 * Inputs:
 *   - filp: The file pointer representing the file to read from.
 *   - buf: The user-space buffer to copy the data into.
 *   - len: The number of bytes to read.
 *   - ppos: The file position pointer.
 * Returns:
 *   - The number of bytes read on success.
 *   - 0 if the end of the file is reached.
 *   - -EFAULT if copying data to user space fails.
 */
static ssize_t osfs_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_read;

    // If the file has not been allocated a data block, it indicates the file is empty
    if (osfs_inode->i_blocks == 0)
        return 0;

    if (*ppos >= osfs_inode->i_size) return 0;
    
    uint32_t logical_block_num = *ppos / BLOCK_SIZE;
    uint32_t block_offset = *ppos % BLOCK_SIZE;
    
    uint32_t phys_block = osfs_get_phys_block(osfs_inode, logical_block_num);
    
    if (phys_block == 0) return 0; 

    data_block = sb_info->data_blocks + phys_block * BLOCK_SIZE + block_offset;
    
    bytes_read = len;
    if (block_offset + len > BLOCK_SIZE) {
        bytes_read = BLOCK_SIZE - block_offset;
    }

    if (copy_to_user(buf, data_block, bytes_read))
        return -EFAULT;

    *ppos += bytes_read;
    return bytes_read;
}


/**
 * Function: osfs_write
 * Description: Writes data to a file.
 * Inputs:
 *   - filp: The file pointer representing the file to write to.
 *   - buf: The user-space buffer containing the data to write.
 *   - len: The number of bytes to write.
 *   - ppos: The file position pointer.
 * Returns:
 *   - The number of bytes written on success.
 *   - -EFAULT if copying data from user space fails.
 *   - Adjusted length if the write exceeds the block size.
 */
static ssize_t osfs_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{   
    //Step1: Retrieve the inode and filesystem information
    struct inode *inode = file_inode(filp);
    struct osfs_inode *osfs_inode = inode->i_private;
    struct osfs_sb_info *sb_info = inode->i_sb->s_fs_info;
    void *data_block;
    ssize_t bytes_written;
    int ret;

    // extent
    uint32_t logical_block_num = *ppos / BLOCK_SIZE;
    uint32_t block_offset = *ppos % BLOCK_SIZE;
    
    // 1. try to find remaining Block
    uint32_t phys_block = osfs_get_phys_block(osfs_inode, logical_block_num);
    
    // 2. If not found then create Extent
    if (phys_block == 0) {
        // Multi-block Allocation Strategy
        uint32_t blocks_needed = (block_offset + len + BLOCK_SIZE - 1) / BLOCK_SIZE; // ideal number of blocks needed for the remaining data
        uint32_t new_phys_block;
        
        // allocate all required blocks at once
        ret = osfs_alloc_extent_block(sb_info, blocks_needed, &new_phys_block);
        if (ret) {
            // [Fallback] If sufficient contiguous space is unavailable, revert to single block allocation
            blocks_needed = 1;
            ret = osfs_alloc_extent_block(sb_info, 1, &new_phys_block);
            if (ret) return ret; // No any space left (ENOSPC)
        }

        // Extent Merging Strategy
        int merged = 0;
        if (osfs_inode->extent_count > 0) {
            struct osfs_extent *last_ext = &osfs_inode->extents[osfs_inode->extent_count - 1];
            
            // check Physical Contiguity and Logical Contiguity
            if ((last_ext->ee_start + last_ext->ee_len == new_phys_block) &&
                (last_ext->ee_block + last_ext->ee_len == logical_block_num)) {                
                // Merge successful: extend the last extent, do not create a new one
                last_ext->ee_len += blocks_needed;
                merged = 1;
            }
        }

        if (!merged) {
            if (osfs_inode->extent_count >= OSFS_MAX_EXTENTS) return -ENOSPC; // Extent array is full
            struct osfs_extent *new_ext = &osfs_inode->extents[osfs_inode->extent_count];
            new_ext->ee_block = logical_block_num;
            new_ext->ee_start = new_phys_block;
            new_ext->ee_len = blocks_needed;
            osfs_inode->extent_count++;
        }
        
        osfs_inode->i_blocks += blocks_needed;
        inode->i_blocks = osfs_inode->i_blocks; // sync VFS inode
        
        phys_block = new_phys_block;        
    }

    // 3. write date
    data_block = sb_info->data_blocks + phys_block * BLOCK_SIZE + block_offset;
    bytes_written = len;
    if (block_offset + len > BLOCK_SIZE) bytes_written = BLOCK_SIZE - block_offset;

    if (copy_from_user(data_block, buf, bytes_written)) return -EFAULT;

    *ppos += bytes_written;
    
    // update file size
    if (*ppos > osfs_inode->i_size) {
        osfs_inode->i_size = *ppos;
        inode->i_size = *ppos;
    }
    
    // update time
    inode_set_mtime_to_ts(inode, current_time(inode));
    inode_set_ctime_to_ts(inode, current_time(inode));

    // mark kernel inode dirty
    mark_inode_dirty(inode);    

    return bytes_written;
}

/**
 * Struct: osfs_file_operations
 * Description: Defines the file operations for regular files in osfs.
 */
const struct file_operations osfs_file_operations = {
    .open = generic_file_open, // Use generic open or implement osfs_open if needed
    .read = osfs_read,
    .write = osfs_write,
    .llseek = default_llseek,
    // Add other operations as needed
};

/**
 * Struct: osfs_file_inode_operations
 * Description: Defines the inode operations for regular files in osfs.
 * Note: Add additional operations such as getattr as needed.
 */
const struct inode_operations osfs_file_inode_operations = {
    // Add inode operations here, e.g., .getattr = osfs_getattr,
};
