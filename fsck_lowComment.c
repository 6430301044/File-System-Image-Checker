#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define stat xv6_custom_stat  // avoid clash with host (struct stat in stat.h)
#include "fs.h"
#include "types.h"
#include "stat.h"
#include "file.h"
#include "param.h"
#include "file.h"
#include "spinlock.h"
#include "sleeplock.h"

struct superblock sb;
struct dinode inode;
struct dirent de;
int fd;
uint buffer[NINDIRECT];

int check_inodes(int fd) {
  for (int inum = 0; inum < sb.ninodes; inum++) {
    if (readi(fd, inum, &inode) > 0) {
        if (inode.type < 0 || inode.type > 4) {
            return 1;
        }
    }
  }
  return 0;
}

int check_block_validity(int fd, uint datablockNum) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (readi(fd, inum, &inode) > 0) {
            for (int i = 0; i < NDIRECT; i++) {
                if (inode.addrs[i] == datablockNum) {
                    return 0; // Block is valid
                }
            } 
            if (inode.addrs[NDIRECT] != 0) {
                uint indirectBuffer[NINDIRECT];
                if (pread(fd, indirectBuffer, BSIZE, inode.addrs[NDIRECT] * BSIZE) < 0) {
                    perror("pread");
                    return 1;
                }
                for (int i = 0; i < NINDIRECT; i++) {
                    if (indirectBuffer[i] == datablockNum) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1; // Invalid block
}

int check_directAddres(int fd){
    for (int inum = 0; inum < NDIRECT; inum++) {
        if (inode.addrs[inum] != 0) {
            if (check_block_validity(fd, inode.addrs[inum]) != 0) {
                return 1; // Invalid direct address in inode
            }
        }
    }
    return 0;
}

int check_indirectAddres(int fd){
    for (int inum = NDIRECT; inum < NINDIRECT; inum++) {
        if (inode.addrs[inum] != 0) {
            if (check_block_validity(fd, inode.addrs[inum]) != 0) {
                return 1; // Invalid indirect address in inode
            }
        }
    }
    return 0;
}

int check_root_directory(int fd) {
    struct dinode root_inode;
    if (readi(fd, 1, &root_inode) < 0) {
        return 1; // Error reading the root directory inode
    }
    if (root_inode.type != T_DIR) {
        return 1; // Root directory does not exist or has the wrong type
    }
    if (root_inode.addrs[0] != 1) {
        return 1; // Root directory's parent is not itself
    }
    return 0; // No errors found
}

int check_directory_format(int fd) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (readi(fd, inum, &inode) > 0) {
            if (inode.type == T_DIR) {
                for (int offset = 0; offset < BSIZE; offset += sizeof(de)) {
                    if (pread(fd, &de, sizeof(de), inode.addrs[0] * BSIZE + offset) < 0) {
                        perror("pread");
                        return 1; // Error reading the directory entry
                    }
                    if (strcmp(de.name, ".") == 0) {
                        if (de.inum != inum) {
                            return 1; // "." entry does not point to itself
                        }
                    } // เปรียบเทียบ directory name กับ ".." [dot-dot]
                    else if (strcmp(de.name, "..") == 0) {
                        if (de.inum != 1) {
                            return 1; // ".." entry does not point to the parent directory
                        }
                    }
                }
            }
        }
    }
    return 0; // No errors found
}

int check_useinodeAndBitmap(int fd) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (readi(fd, inum, &inode) > 0) {
            if (inode.type == 0) {
                return 1; // มีข้อผิดพลาดเพราะ inode ไม่ถูกใช้งาน
            }
            struct buf *bp = bread(fd, BBLOCK(inum, sb));
            uchar *bitmap = (uchar *)bp->data;
            int bit = inum % BPB; // หาว่าคือ bit ที่ต้องการตรวจสอบใน byte
            int byte = bit / 8;   // หาว่าอยู่ใน byte ที่เท่าไร
            bit = bit % 8;       // หาว่าอยู่ใน bit ที่เท่าไร
            if ((bitmap[byte] & (1 << bit)) == 0) {
                return 1; // มีข้อผิดพลาดเพราะ bitmap ไม่ถูกต้อง
            }
            brelse(bp); // คืน memory buffer หลังใช้เสร็จ
        }
    }
    return 0; // ไม่มีข้อผิดพลาด
}

int check_block_usage(int fd, int datablockNum) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (readi(fd, inum, &inode) > 0) {
            for (int i = 0; i < NDIRECT; i++) {
                if (inode.addrs[i] == datablockNum) {
                    return 1; // Block is in use by an inode
                }
            }
            if (inode.addrs[NDIRECT] != 0) {
                uint indirectBuffer[NINDIRECT];
                if (pread(fd, indirectBuffer, BSIZE, inode.addrs[NDIRECT] * BSIZE) < 0) {
                    perror("pread");
                    return 0;
                }
                for (int i = 0; i < NINDIRECT; i++) {
                    if (indirectBuffer[i] == datablockNum) {
                        //ถ้าใช้ ให้คืนค่า 1 ถ้าไม่
                        return 1; // Block is in use by an indirect block
                    }
                }
            }
        }
    }
    return 0; // Block is not in use
}
int check_blocks_in_use(int fd) {
    for (int datablockNum = 0; datablockNum < sb.size; datablockNum++) {
        if (lseek(fd, sb.bmapstart * BSIZE + datablockNum / 8, SEEK_SET) != sb.bmapstart * BSIZE + datablockNum / 8) {
            perror("lseek"); // แสดงข้อความข้อผิดพลาดถ้ามีปัญหาในการใช้ lseek
            return 1;
        }
        uchar bitmapByte;
        if (read(fd, &bitmapByte, 1) != 1) {
            perror("read");
            return 1;
        }
        int bitPos = datablockNum % 8;
        int bitValue = (bitmapByte >> bitPos) & 1;
        if (bitValue == 1) {
            int blockInUse = check_block_usage(fd, datablockNum);
            if (blockInUse == 0) {
                return 1; // ถ้าไม่ถูกใช้งาน ให้คืนค่า 1 แสดง ERROR ถ้าถูกใช้งานให้ออกจากเงื่อนไข
            }
        }
    }
    return 0; // No errors found
}

int check_direct_address_usage(int fd) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (readi(fd, inum, &inode) > 0) {
            int usedDirectAddresses[NDIRECT];
            for (int i = 0; i < NDIRECT; i++) {
                usedDirectAddresses[i] = 0; // ใส่ค่า 0 ลงไปตามขนาดของอาร์เรย์เพื่อใช้แทนค่าว่า inode ที่ตำแหน่งนี้ไม่มีการใช้งานเป็นค่า Free
            }
            for (int i = 0; i < NDIRECT; i++) {
                if (inode.addrs[i] != 0) {
                    if (usedDirectAddresses[i] == 1) {
                        return 1; // Direct address used more than once
                    } 
                    else {
                        usedDirectAddresses[i] = 1; // Mark the address as used
                    }
                }
            }
        }
    }
    return 0; // No errors found
}

int check_indirect_address_usage(int fd) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (readi(fd, inum, &inode) > 0) {
            int usedIndirectAddresses[NINDIRECT];
            for (int i = NDIRECT; i < NINDIRECT; i++) {
                usedIndirectAddresses[i] = 0; // ใส่ค่า 0 ลงไปตามขนาดของอาร์เรย์เพื่อใช้แทนค่าว่า inode ที่ตำแหน่งนี้ไม่มีการใช้งานเป็นค่า Free
            }
            if (inode.addrs[NDIRECT] != 0) {
                if (pread(fd, buffer, BSIZE, inode.addrs[NDIRECT] * BSIZE) < 0) {
                    return 1; // Error reading the indirect block
                }
                for (int i = 0; i < NINDIRECT; i++) {
                    if (buffer[i] != 0) {
                        if (usedIndirectAddresses[i] == 1) {
                            return 1; // Indirect address used more than once
                        } 
                        else {
                            usedIndirectAddresses[i] = 1; // Mark the address as used
                        }
                    }
                }
            }
        }
    }
    return 0; // No errors found
}

int main(int argc, char *argv[]){
    if (argc != 2)
    {
        printf("Usage: fs.img\n");
        exit(1);
    }
    
    fd = open(argv[1],O_RDONLY);

    //1
    if (check_inodes(fd) == 1)
    {
        close(fd);
        printf("ERROR: bad inode.\n");
        exit(1);
    }
    //2.1
    if (check_directAddres(fd) == 1)
    {
        close(fd);
        printf("ERROR: bad direct address in inode.\n");
        exit(1);
    }
    //2.2
    if (check_indirectAddres(fd) == 1)
    {
        close(fd);
        printf("ERROR: bad indirect address in inode.\n");
        exit(1);
    }
    //3
    if (check_root_directory(fd) == 1)
    {
        close(fd);
        printf("ERROR: root directory does not exist.\n");
        exit(1);
    }
    //4
    // Read the superblock
    if (pread(fd, &sb, sizeof(sb), BSIZE) < 0) {
        perror("pread");
        exit(1);
    }
    if (check_directory_format(fd) == 1) 
    {
        close(fd);
        printf("ERROR: directory not properly formatted.\n");
        exit(1);
    }
    //5
    if (check_useinodeAndBitmap(fd) == 1)
    {
        close(fd);
        printf("ERROR: address used by inode but marked free in bitmap.\n");
        exit(1);
    }
    //6
    if (check_blocks_in_use(fd) == 1)
    {
        close(fd);
        printf("ERROR: bitmap marks block in use but it is not in use.\n");
        exit(1);
    }
    //7
    if (check_direct_address_usage(fd) == 1)
    {
        close(fd);
        printf("ERROR: direct address used more than once in inode.\n");
        exit(1);
    }
    //8
    if (check_indirect_address_usage(fd) == 1)
    {
        close(fd);
        printf("ERROR: indirect address used more than once in inode.\n");
        exit(1);
    }

    close(fd);
    printf("File System Image NO ERROR.\n");
    exit(0);
}