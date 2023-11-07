#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define stat xv6_custom_stat  // avoid clash with host (struct stat in stat.h)
#include "types.h"
#include "fs.h"
#include "stat.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "buf.h"


struct superblock sb;
struct dinode inode;
struct dirent de;
int fd;
uint buffer[NINDIRECT];

//1
int check_inodes() {
  struct dinode inode;
  char buf[sizeof(struct dinode)];
  lseek(fd,sb.inodestart*BSIZE,SEEK_SET);
  for (int inum = 0; inum < sb.ninodes; inum++) {
    // ตรวจสอบไฟล์ใน inode
    if (lseek(fd, (inum + 1) * sizeof(struct dinode), SEEK_SET) < 0) {
      perror("lseek");
      return 1;
    }
    
    if (read(fd, &inode, sizeof(struct dinode)) != sizeof(struct dinode)) {
      perror("read");
      return 1;
    }

    if (inode.type < 1 || inode.type > 3) {
      // ตรวจสอบประเภทไฟล์และคืนค่า 1 ถ้าไม่ตรง
      return 1;
    }
  }

  return 0;
}

//2 Function 2
int check_block_validity(int fd, uint datablockNum) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        // Seek to the beginning of the inode
        off_t offset = BSIZE * BBLOCK(inum, sb);
        if (lseek(fd, offset, SEEK_SET) < 0) {
            perror("lseek");
            return 1;
        }

        if (read(fd, &inode, sizeof(struct dinode)) != sizeof(struct dinode)) {
            perror("read");
            return 1;
        }

        for (int i = 0; i < NDIRECT; i++) {
            if (inode.addrs[i] == datablockNum) {
                return 0; // Block is valid
            }
        }

        if (inode.addrs[NDIRECT] != 0) {
            // Seek to the beginning of the indirect block
            offset = inode.addrs[NDIRECT] * BSIZE;
            if (lseek(fd, offset, SEEK_SET) < 0) {
                perror("lseek");
                return 1;
            }

            uint indirectBuffer[NINDIRECT];
            if (read(fd, indirectBuffer, BSIZE) != BSIZE) {
                perror("read");
                return 1;
            }

            for (int i = 0; i < NINDIRECT; i++) {
                if (indirectBuffer[i] == datablockNum) {
                    return 0; // Block is valid
                }
            }
        }
    }
    return 1; // Invalid block
}

//2.1 Function 1
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

//2.2 Function 1
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

//3
int check_root_directory(int fd) {
    struct dinode root_inode;

    // Seek to the beginning of the root inode
    off_t offset = BSIZE * BBLOCK(1, sb);
    if (lseek(fd, offset, SEEK_SET) < 0) {
        perror("lseek");
        return 1;
    }

    if (read(fd, &root_inode, sizeof(struct dinode)) != sizeof(struct dinode)) {
        perror("read");
        return 1;
    }

    if (root_inode.type != T_DIR) {
        return 1;
    }

    if (root_inode.addrs[0] != 1) {
        return 1;
    }

    return 0;
}

//4
int check_directory_format(int fd) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (lseek(fd, IBLOCK(inum, sb), SEEK_SET) < 0) {
            perror("lseek");
            return 1;
        }
        
        if (read(fd, &inode, sizeof(inode)) != sizeof(inode)) {
            perror("read");
            return 1;
        }

        if (inode.type == T_DIR) {
            for (int offset = 0; offset < BSIZE; offset += sizeof(de)) {
                if (lseek(fd, inode.addrs[0] * BSIZE + offset, SEEK_SET) < 0) {
                    perror("lseek");
                    return 1;
                }
                
                if (read(fd, &de, sizeof(de)) != sizeof(de)) {
                    perror("read");
                    return 1;
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
    return 0; // No errors found
}

//5
int check_useinodeAndBitmap(int fd) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (lseek(fd, BBLOCK(inum, sb) * BSIZE, SEEK_SET) < 0) {
            perror("lseek");
            return 1; // Error seeking to the bitmap block
        }

        uchar bitmap[BSIZE];
        if (read(fd, bitmap, BSIZE) < 0) {
            perror("read");
            return 1; // Error reading the bitmap block
        }

        int bit = inum % BPB;
        int byte = bit / 8;
        bit = bit % 8;
        if ((bitmap[byte] & (1 << bit)) == 0) {
            return 1; // Address used by inode but marked free in bitmap
        }
    }
    return 0; // No errors found
}

//6 Function 2
int check_block_usage(int fd, int datablockNum) {
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (lseek(fd, IBLOCK(inum, sb) * BSIZE, SEEK_SET) < 0) {
            perror("lseek");
            return 1;
        }

        if (read(fd, &inode, sizeof(inode)) != sizeof(inode)) {
            perror("read");
            return 1;
        }

        for (int i = 0; i < NDIRECT; i++) {
            if (inode.addrs[i] == datablockNum) {
                return 1; // Block is in use by an inode
            }
        }

        if (inode.addrs[NDIRECT] != 0) {
            uint indirectBuffer[NINDIRECT];
            if (lseek(fd, inode.addrs[NDIRECT] * BSIZE, SEEK_SET) < 0) {
                perror("lseek");
                return 1;
            }
            if (read(fd, indirectBuffer, BSIZE) != BSIZE) {
                perror("read");
                return 1;
            }
            for (int i = 0; i < NINDIRECT; i++) {
                if (indirectBuffer[i] == datablockNum) {
                    return 1; // Block is in use by an indirect block
                }
            }
        }
    }
    return 0; // Block is not in use
}

//6 Function 1
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

//7
//ตรวจสอบ inode ที่ใช้งานอยู่ว่ามี data block ที่ชี้มายัง direct address มากกว่า 1 datablock หรือไม่ ถ้าเป็นจริง ให้แสดงข้อความ ERROR: direct address used more than once.
int check_direct_address_usage(int fd) {
    // วนลูปตรวจสอบ inode ทุกตัว
    for (int inum = 0; inum < sb.ninodes; inum++) {
        //File ที่อ่าน,offset ตำแหน่งที่จะอ่าน, where set ตามตำแหน่งที่จะได้,cur ตำแหน่งปัจจุบัน,end ตำแหน่งสุดท้ายของไฟล์
        if (lseek(fd, IBLOCK(inum, sb) * BSIZE, SEEK_SET) < 0) {
            perror("lseek");
            return 1;
        }
        //File ที่อ่านตำแหน่งใน, memory buffer, count จำนวนข้อมูล
        if (read(fd, &inode, sizeof(inode)) != sizeof(inode)) {
            perror("read");
            return 1;
        }
        // สร้างอาร์เรย์ไว้สำหรับเก็บ Track เพื่อใช้ในการเปรียบเทียบ
        int usedDirectAddresses[NDIRECT];
        // ใส่ค่า 0 ลงไปตามขนาดของอาร์เรย์เพื่อใช้แทนค่าว่า inode ที่ตำแหน่งนี้ไม่มีการใช้งานเป็นค่า Free
        for (int i = 0; i < NDIRECT; i++) {
            usedDirectAddresses[i] = 0;
        }
        // วนลูปตรวจสอบ direct address ใน inode ทุกตัว
        for (int i = 0; i < NDIRECT; i++) {
            if (inode.addrs[i] != 0) {
                // ตรวจสอบว่า direct address นั้นถูกใช้งานมาก่อนหรือยัง ถ้าใช้แล้วให้คืนค่า 1
                if (usedDirectAddresses[i] == 1) {
                    return 1; // Direct address used more than once
                } 
                else {//แต่ถ้ายังไม่ถูกใช้งานให้ Mark direct address นั้นให้มีค่าเท่ากับ 1
                    usedDirectAddresses[i] = 1; // Mark the address as used
                }
            }
        }
        //ถ้าวนจนครบทุก inode ของ direct address แล้ว ให้ออกจาก for loop
    }
    //แล้วคืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่อง inode ที่ใช้งานอยู่ว่ามี data block ที่ชี้มายัง direct address มากกว่า 1 datablock
    return 0; // No errors found
}

//8
//ตรวจสอบ inode ที่ใช้งานอยู่ว่ามี data block ที่ชี้มายัง indirect address มากกว่า 1 datablock หรือไม่ ถ้าเป็นจริง ให้แสดงข้อความ ERROR: indirect address used more than once.
int check_indirect_address_usage(int fd) {
    char buffer[BSIZE]; // Declare a buffer to read the indirect block
    // วนลูปตรวจสอบ inode ทุกตัว
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (lseek(fd, IBLOCK(inum, sb) * BSIZE, SEEK_SET) < 0) {
            perror("lseek");
            return 1;
        }

        if (read(fd, &inode, sizeof(inode)) != sizeof(inode)) {
            perror("read");
            return 1;
        }
        // สร้างอาร์เรย์ไว้สำหรับเก็บ Track เพื่อใช้ในการเปรียบเทียบ
        int usedIndirectAddresses[NINDIRECT];
        for (int i = NDIRECT; i < NINDIRECT; i++) {
            usedIndirectAddresses[i] = 0;
        }

        if (inode.addrs[NDIRECT] != 0) {
            if (lseek(fd, inode.addrs[NDIRECT] * BSIZE, SEEK_SET) < 0) {
                perror("lseek");
                return 1;
            }
            if (read(fd, buffer, BSIZE) != BSIZE) {
                perror("read");
                return 1;
            }
            // วนลูปตรวจสอบ indirect address ใน inode ทุกตัว
            for (int i = 0; i < NINDIRECT; i++) {
                // ตรวจสอบว่า indirect address นั้นถูกใช้งานมาก่อนหรือยัง ถ้าใช้แล้วให้คืนค่า 1 
                if (buffer[i] != 0) {
                    if (usedIndirectAddresses[i] == 1) {
                        return 1; // Indirect address used more than once
                    } 
                    else {//แต่ถ้ายังไม่ถูกใช้งานให้ Mark indirect address นั้นให้มีค่าเท่ากับ 1
                        usedIndirectAddresses[i] = 1; // Mark the address as used
                    }
                }
            }
        }
        //ถ้าวนจนครบทุก inode ของ direct address แล้ว ให้ออกจาก for loop
    }
    //แล้วคืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่อง inode ที่ใช้งานอยู่ว่ามี data block ที่ชี้มายัง indirect address มากกว่า 1 datablock
    return 0; // No errors found
}

int main(int argc, char *argv[]){
    if (argc != 2)
    {
        printf("Usage: fs.img\n");
        exit(1);
    }
    
    fd = open(argv[1],O_RDONLY);

    int errors = 0; // เพิ่มตัวแปรเพื่อเก็บข้อผิดพลาด

    //1
    if (check_inodes() == 1)
    {
        printf("ERROR: bad inode.\n");
        errors++;
    }
    //2.1
    if (check_directAddres(fd) == 1)
    {
        printf("ERROR: bad direct address in inode.\n");
        errors++;
    }
    //2.2
    if (check_indirectAddres(fd) == 1)
    {
        printf("ERROR: bad indirect address in inode.\n");
        errors++;
    }
    //3
    if (check_root_directory(fd) == 1)
    {
        printf("ERROR: root directory does not exist.\n");
        errors++;
    }
    //4
    // Read the superblock
    if (pread(fd, &sb, sizeof(sb), BSIZE) < 0) {
        perror("pread");
        exit(1);
    }
    if (check_directory_format(fd) == 1) 
    {
        printf("ERROR: directory not properly formatted.\n");
        errors++;
    }
    //5
    if (check_useinodeAndBitmap(fd) == 1)
    {
        printf("ERROR: address used by inode but marked free in bitmap.\n");
        errors++;
    }
    //6
    if (check_blocks_in_use(fd) == 1)
    {
        printf("ERROR: bitmap marks block in use but it is not in use.\n");
        errors++;
    }
    //7
    if (check_direct_address_usage(fd) == 1)
    {
        printf("ERROR: direct address used more than once in inode.\n");
        errors++;
    }
    //8
    if (check_indirect_address_usage(fd) == 1)
    {
        printf("ERROR: indirect address used more than once in inode.\n");
        errors++;
    }

    close(fd);

    if (errors == 0) {
        printf("File System Image NO ERROR.\n");
        exit(0);
    } else {
        exit(1);
    }
}