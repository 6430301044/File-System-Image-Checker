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

//ตรวจสอบ inode ทุกตัวที่มีการใช้งาน [ถูกจัดสรรพื้นที่ให้] และมี Type นอกเหนือจาก DIR, Regura, Device ให้แสดงข้อความ ERROR: bad inode.
int check_inodes(int fd) {
  // วนลูปเช็คทุก inode เริ่มต้นที่ศูนย์ inode น้อยกว่า จำนวน inode ทั้งหมดบนดิสก์ ให้ inode เพิ่มขึ้นที่ละ 1
  for (int inum = 0; inum < sb.ninodes; inum++) {
    /*ตรวจสอบไฟล์ fd ว่าตำแหน่งที่ถูกอ่านนั้นมีไฟล์หรือไม่ ถ้ามี ไฟล์ ก็จะต้องมีขนาดไฟล์ ชนิดไฟล์ และระบบจะต้องจัดสรร inode ให้ ดังนั้นไฟล์ที่ตำแหน่งนั้้นจึงเข้าเงื่อนไข เพราะไม่ใช่ไฟล์ว่าง[Free File] หรือไฟล์ที่ไม่ได้ถูกจัดสรรพื้นที่ให้ [unalloction]
    readi(ใช้ไฟล์นี้อ่านข้อมูลจาก disk, inum เป็นตำแหน่งที่จะถูกอ่านค่า, เป็น pointer* ที่คอยชี้ที่ต้องไปเก็บข้อมูลไว้ ข้อมูลที่ได้เกี่ยวกับ inode ที่เก็บไว้ใน address& โครงสร้างของ disk inode
    ข้อมูลที่เก็บได้ เช่น ประเภทไฟล์ ตำแหน่งและขนาดของข้อมูลที่จะถูกอ่านจาก disk image)*/
    if (readi(fd, inum, &inode) > 0) {
        /*xv6 มีชนิดไฟล์ 3 ชนิด 1.Directory file 2.Regular File 3.Device File
        เช็คเงื่อนไขว่า inode ชนิดของไฟล์ มีค่าน้อยกว่าหรือมากกว่า 4 ไหม เพราะ xv6 มีค่าคงที่ให้ไฟล์คือค่า 1, 2, 3 ตามลำดับ
        ถ้าเงื่อนไขตรงให้ คืนค่า 1 กลับไป*/
        if (inode.type < 0 || inode.type > 4) {
            return 1;
        }
    }
  }
  //ถ้าเช็ค inode ครบทุกตัวแล้ว ไม่มี inode ไหนตรงตามเงื่อนไข จะออกจาก for loop แล้ว คืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่องชนิดของไฟล์
  return 0;
}

//ตรวจสอบความถูกต้องของ data block
int check_block_validity(int fd, uint datablockNum) {
    // Check if the block is used by an inode
    for (int inum = 0; inum < sb.ninodes; inum++) {
        if (readi(fd, inum, &inode) > 0) {
            for (int i = 0; i < NDIRECT; i++) {
                //ตรวจสอบว่า address ของ direct address ที่ inode ชี้ไปถูกต้องหรือไม่ ถ้าถูกให้คืนค่า 0 ถ้าไม่ให้ออกจาก for loop แล้ว ไปตรวจสอบต่อว่าเป็น indirect address หรือเปล่า
                if (inode.addrs[i] == datablockNum) {
                    return 0; // Block is valid
                }
            }
            //ตรวจสอบว่า address ของ indirect address ถ้าไม่มีค่าเท่ากับ 0  
            if (inode.addrs[NDIRECT] != 0) {
                //สร้างอาร์เรย์สำหรับใช้เก็บค่าจากไฟล์เพื่อนำไปใช้เปรียบเทียบ
                uint indirectBuffer[NINDIRECT];
                //โดยตรวจสอบต่อว่าตำแหน่งไฟล์ที่อ่านมีค่าน้อยกว่า 0 หรือไม่ ถ้าน้อยกว่าให้คืนค่า 1 ถ้าไม่
                //pread(อ่านข้อมูลจาก, pointer* ชี้ว่าต้องเก็บข้อมูลที่อ่านได้ไว้ในตัวแปรอะไร, ข้อมูลที่อ่านมีขนาดเท่าไร, ตำแหน่งของไฟล์ที่ต้องการอ่านข้อมูล)
                if (pread(fd, indirectBuffer, BSIZE, inode.addrs[NDIRECT] * BSIZE) < 0) {
                    //perror() เป็นฟังก์ชั่นที่ใช้แสดงข้อผืดพลาดที่เกิดขึ้นในระบบ
                    perror("pread");
                    return 1;
                }
                //ให้ตรวจสอบว่า datablockNum อยู่ใน indirectBuffer หรือไม่
                for (int i = 0; i < NINDIRECT; i++) {
                    //ถ้าใช้ ให้คืนค่า 0 ถ้าไม่ 
                    if (indirectBuffer[i] == datablockNum) {
                        return 0; // Block is valid
                    }
                }
            }
        }
    }
    //คืนค่า 1
    return 1; // Invalid block
}

//ตรวจสอบว่าทุก address ที่ถูกใช้โดย inode นั้นชี้ไปยัง address ของ datablock ที่ตรงกับของตัวเองไหม
//ถ้าไม่แสดงข้อความ ERROR: bad direct address in inode.
int check_directAddres(int fd){
    // for loop ตรวจสอบ inode ทุกตัว
    for (int inum = 0; inum < NDIRECT; inum++) {
        //ถ้า inode ไม่เท่ากับ 0
        if (inode.addrs[inum] != 0) {
            //ให้ตรวจสอบ direct address ที่ชี้ไปยัง datablock นั้น ถ้าค่าที่ คืนมาไม่ใช่ 0 ให้คืนค่า 0 ไป แต่ถ้าเป็น 0 ให้ออกจาก for loop 
            if (check_block_validity(fd, inode.addrs[inum]) != 0) {
                return 1; // Invalid direct address in inode
            }
        }
    }
    //แล้วคืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่อง address ที่ถูกใช้โดย inode นั้นชี้ไปยัง address ของ datablock ที่ตรงกับของตัวเองไหม [Direct Address]
    return 0;
}

//ถ้าไม่แสดงข้อความ ERROR: bad indirect address in inode.
int check_indirectAddres(int fd){
    // for loop ตรวจสอบ inode ทุกตัว
    for (int inum = NDIRECT; inum < NINDIRECT; inum++) {
        //ถ้า inode ไม่เท่ากับ 0
        if (inode.addrs[inum] != 0) {
            //ให้ตรวจสอบ indirect address ที่ชี้ไปยัง datablock นั้น ถ้าค่าที่ คืนมาไม่ใช่ 0 ให้คืนค่า 0 ไป แต่ถ้าเป็น 0 ให้ออกจาก for loop 
            if (check_block_validity(fd, inode.addrs[inum]) != 0) {
                return 1; // Invalid indirect address in inode
            }
        }
    }
    //แล้วคืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่อง address ที่ถูกใช้โดย inode นั้นชี้ไปยัง address ของ datablock ที่ตรงกับของตัวเองไหม [Indirect Address]
    return 0;
}

//ตรวจสอบว่า Root Directory มี inode เป็น 1 และ Parent directory เป็นตัวมันเองไหม ถ้าไม่แสดงข้อความ ERROR: root directory does not exist.
int check_root_directory(int fd) {
    struct dinode root_inode;
    //อ่านไฟล์ fd ที่ inode 1 แล้วเก็บไว้ใน pointer* ของ stucter disk-inode root_inode ถ้าค่าที่ได้น้อยกว่า 0 ให้คืนค่า 1
    if (readi(fd, 1, &root_inode) < 0) {
        return 1; // Error reading the root directory inode
    }

    // ตรวจสอบชนิดของไฟล์ต่อถ้าไม่ใช่ 1 หรือ Directory File ให้คืนค่า 1
    if (root_inode.type != T_DIR) {
        return 1; // Root directory does not exist or has the wrong type
    }

    // ตรวจสอบว่า Root Directory เป็น Parent Directory ด้วยตัวมันเองไหม (inode number เท่ากับ 1) ถ้าใช่คืนค่า 1
    if (root_inode.addrs[0] != 1) {
        return 1; // Root directory's parent is not itself
    }

    //ถ้าไม่ คืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่อง Root Directory มี inode เป็น 1 และ Parent directory เป็นตัวมันเองไหม
    return 0; // No errors found
}

//ตรวจสอบว่า Directory มี directory ชื่อ "."[dot] กับ ".."[dot-dot] ไหม โดยที่ถ้า dot ใน directory จะต้องชี้ไปที่ directory ปัจจุบัน[ชี้ที่ directory ตัวเอง] ถ้า dot-dot ให้ชี้ไปยัง Parent directory
//ถ้าไม่แสดงข้อความ ERROR: directory not properly formatted.
int check_directory_format(int fd) {
    // ตรวจสอบ inode ทุกตัว
    for (int inum = 0; inum < sb.ninodes; inum++) {
        // ตรวจสอบว่า inode นั้นเป็นไฟล์ที่มี data block หรือเปล่า [Allocation File]
        if (readi(fd, inum, &inode) > 0) {
            // ตรวจสอบว่า inode นั้น มีชนิดไฟล์เป็น Directory File ไหม
            if (inode.type == T_DIR) {
                // For loop ใน Directory 
                for (int offset = 0; offset < BSIZE; offset += sizeof(de)) {
                    /*อ่านข้อมูลจากไฟล์ fd pointer* เก็บไว้ใน address& ของ structer directory de ขนาดของข้อมูลเท่ากับ sizeof ของ de, offset เท่ากับ  inode.addrs[0] * BSIZE + offset 
                    น้อยกว่า 0 ถ้าเงื่อนไขเป็นจริง แสดงว่า เกิดข้อผิดพลาดในการอ่านไฟล์ โค้ดเลยไม่สามารถอ่านไฟล์ได้*/
                    if (pread(fd, &de, sizeof(de), inode.addrs[0] * BSIZE + offset) < 0) {
                        perror("pread");
                        return 1; // Error reading the directory entry
                    }

                    /* ตรวจสอบ directory name "." [dot] โดยการนำชื่อมาเปรียบเทียบกัน [เปรียบเทียบ String] 
                    ถ้าทั้งสองคำเหมือนกันจะมีค่าเท่ากับ 0 ถ้าตัวแรกมากกว่าตัวสองจะมีค่าเป็นบวกตามตัวอักษรที่เยอะกว่า ถ้าน้อยกว่าจะมีค่าเป็นลบ
                    แต่ถ้าเป็นคนละคำแต่จำนวนตัวอักษรเท่ากัน ก็จะเปรียบเทียบว่าตัวอักษรนั้นตามแต่ละตำแหน่งนั้นคำไหนมีค่ามากกว่ากัน เช่น
                    abc กับ def strcmp จะได้ผลลบเพราะ a มากกว่า d ตามตำแหน่งแรกที่เป็นตัวแรกของสตริง
                    ถ้าเป็น xyz กับ xzz strcmp จะได้ผลลบเหมือนกันเพราะ y มากกว่า z.*/
                    if (strcmp(de.name, ".") == 0) {
                        //ตรวจสอบว่า inode ไม่เท่ากับ inode ปัจจุบันไหม [inode ตัวเอง] ถ้าใช่คืนค่า 1
                        if (de.inum != inum) {
                            return 1; // "." entry does not point to itself
                        }
                    } // เปรียบเทียบ directory name กับ ".." [dot-dot]
                    else if (strcmp(de.name, "..") == 0) {
                        //ตรวจสอบว่า inode ไม่เท่ากับ Parent Directory ไหม (inode number เท่ากับ 1) ถ้าใช่คืนค่า 1 ถ้าไม่ออกจาก for loop
                        if (de.inum != 1) {
                            return 1; // ".." entry does not point to the parent directory
                        }
                    }
                }
            }
        }
        //ถ้าวนจนครบทุก inode ที่เป็น Directory File แล้ว ให้ออกจาก for loop
    }
    //แล้วคืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่อง Directory dot กับ dot-dot 
    return 0; // No errors found
}

//ตรวจสอบว่าทุก address ที่ใช้โดย inode นั้นถูกทำเครื่องหมายไว้มีข้อมูลแล้ว[Marked allocation] ถ้าไม่แสดงข้อความ ERROR: address used by inode but marked free in bitmap.
int check_useinodeAndBitmap(int fd) {
    // วนลูปผ่านทุก inode ที่ต้องการตรวจสอบ
    for (int inum = 0; inum < sb.ninodes; inum++) {
        // ตรวจสอบว่า inode นั้นเป็นไฟล์ที่มี data block หรือเปล่า [Allocation File]
        if (readi(fd, inum, &inode) > 0) {
            /* ตรวจสอบว่า inode ณ ตำแหน่ง inum มีการใช้งานหรือไม่
            ทำไมต้อง Type เท่ากับ 0 เพราะ ถึงแม้ว่าจะไม่ใช่ไฟล์ตามโครงสร้างของ xv6 แต่ไฟล์บางประเภทก็ Free [ว่าง] และ ไม่ได้ใช้งาน แต่ พร้อมใช้งานไฟล์ในอนาคต ไฟล์ประเภทนี้จะมีค่า Type เท่ากับ 0*/
            if (inode.type == 0) {
                return 1; // มีข้อผิดพลาดเพราะ inode ไม่ถูกใช้งาน
            }

            // อ่านค่าใน bitmap ที่เกี่ยวข้องกับ inode
            // อ่าน bitmap จาก disk image
            struct buf *bp = bread(fd, BBLOCK(inum, sb));
            uchar *bitmap = (uchar *)bp->data;
            int bit = inum % BPB; // หาว่าคือ bit ที่ต้องการตรวจสอบใน byte
            int byte = bit / 8;   // หาว่าอยู่ใน byte ที่เท่าไร
            bit = bit % 8;       // หาว่าอยู่ใน bit ที่เท่าไร

            // ตรวจสอบค่าใน bitmap
            if ((bitmap[byte] & (1 << bit)) == 0) {
                return 1; // มีข้อผิดพลาดเพราะ bitmap ไม่ถูกต้อง
            }
            brelse(bp); // คืน memory buffer หลังใช้เสร็จ
        }
    }
    //แล้วคืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่อง address ที่ถูกใช้โดย inode นั้นถูกทำเครื่องหมายฟรี ทั้งๆที่ใช้อยู่
    return 0; // ไม่มีข้อผิดพลาด
}

//ตรวจสอบว่า bit ของ Data block ใน bitmap  นั้นมีการใช้งานจริงๆอยู่ไม่
int check_block_usage(int fd, int datablockNum) {
    // ตรวจสอบทุก inode
    for (int inum = 0; inum < sb.ninodes; inum++) {
        //ตรวจสอบว่า inode นั้นเป็นไฟล์ที่มี data block หรือเปล่า [Allocation File] โดยการอ่านข้อมูลจากไฟล์ fd ที่ตำแหน่ง inum เก็บไว้ใน address inode
        if (readi(fd, inum, &inode) > 0) {
            //ตรวจสอบภายใน direct address 0 ถึง 11
            for (int i = 0; i < NDIRECT; i++) {
                //ถ้าพบว่าค่าของ bit มีค่าเท่ากับ datablock ให้คืนค่า 1 เพราะ block นี้มีการใช้งานแล้ว
                if (inode.addrs[i] == datablockNum) {
                    return 1; // Block is in use by an inode
                }
            }
            //ตรวจสอบว่า address ของ indirect address ถ้าไม่มีค่าเท่ากับ 0
            if (inode.addrs[NDIRECT] != 0) {
                //สร้างอาร์เรย์สำหรับใช้เก็บค่าจากไฟล์เพื่อนำไปใช้เปรียบเทียบ
                uint indirectBuffer[NINDIRECT];
                //โดยตรวจสอบต่อว่าตำแหน่งไฟล์ที่อ่านมีค่าน้อยกว่า 0 หรือไม่ ถ้าน้อยกว่าให้คืนค่า 0 ถ้าไม่
                if (pread(fd, indirectBuffer, BSIZE, inode.addrs[NDIRECT] * BSIZE) < 0) {
                    perror("pread");
                    return 0;
                }
                //ตรวจสอบว่า datablockNum อยู่ใน indirectBuffer หรือไม่
                for (int i = 0; i < NINDIRECT; i++) {
                    if (indirectBuffer[i] == datablockNum) {
                        //ถ้าใช้ ให้คืนค่า 1 ถ้าไม่
                        return 1; // Block is in use by an indirect block
                    }
                }
            }
        }
    }
    //คืนค่า 0
    return 0; // Block is not in use
}

/*ตรวจสอบ bit ของ datablock ใน bitmap ว่ามีการใช้งานอยู่ไหม ถ้ามี [allocation] มีค่าเป็น 1 ถ้าไม่มี [Free] ไม่ได้ถูกใช้ง่ายมีค่าเป็น 0
แต่ถ้าพบว่า bit ของ datablock ใน bitmap มีค่าเป็น 1 แต่ data block มีค่าเป็น 0 ให้แสดงข้อความ ERROR: bitmap marks block in use but it is not in use.*/
int check_blocks_in_use(int fd) {
    // ตรวจสอบแต่ละ bit ของ data block ใน bitmap
    for (int datablockNum = 0; datablockNum < sb.size; datablockNum++) {
        // ตรวจสอบโดยการใช้ lseek เพื่อไปยังตำแหน่งที่มีบิตที่เราต้องการตรวจสอบ
        if (lseek(fd, sb.bmapstart * BSIZE + datablockNum / 8, SEEK_SET) != sb.bmapstart * BSIZE + datablockNum / 8) {
            perror("lseek"); // แสดงข้อความข้อผิดพลาดถ้ามีปัญหาในการใช้ lseek
            return 1;
        }
        uchar bitmapByte;
        if (read(fd, &bitmapByte, 1) != 1) {
            perror("read");
            return 1;
        }

        // ถอดบิต Bit สำหรับ data block ปัจจุบัน
        int bitPos = datablockNum % 8;
        int bitValue = (bitmapByte >> bitPos) & 1;

        // ตรวจสอบว่า Bit ถูกตั้งค่าเป็น 1
        if (bitValue == 1) {
            // ตรวจสอบว่า block นี้ถูกใช้งานโดย inode หรือ indirect block
            int blockInUse = check_block_usage(fd, datablockNum);
            if (blockInUse == 0) {
                return 1; // ถ้าไม่ถูกใช้งาน ให้คืนค่า 1 แสดง ERROR ถ้าถูกใช้งานให้ออกจากเงื่อนไข
            }
        }
        //ถ้าวนจนครบทุก bit ของ data block ใน bitmap แล้ว ให้ออกจาก for loop
    }
    //แล้วคืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่อง bit ของ datablock ใน bitmap มีค่าเป็น 1 แต่ data block มีค่าเป็น 0
    return 0; // No errors found
}

//ตรวจสอบ inode ที่ใช้งานอยู่ว่ามี data block ที่ชี้มายัง direct address มากกว่า 1 datablock หรือไม่ ถ้าเป็นจริง ให้แสดงข้อความ ERROR: direct address used more than once.
int check_direct_address_usage(int fd) {
    // วนลูปตรวจสอบ inode ทุกตัว
    for (int inum = 0; inum < sb.ninodes; inum++) {
        // ตรวจสอบว่า inode นั้นเป็นไฟล์ที่มี data block หรือเปล่า [Allocation File]
        if (readi(fd, inum, &inode) > 0) {
            // สร้างอาร์เรย์ไว้สำหรับเก็บ Track เพื่อใช้ในการเปรียบเทียบ
            int usedDirectAddresses[NDIRECT];
            for (int i = 0; i < NDIRECT; i++) {
                usedDirectAddresses[i] = 0; // ใส่ค่า 0 ลงไปตามขนาดของอาร์เรย์เพื่อใช้แทนค่าว่า inode ที่ตำแหน่งนี้ไม่มีการใช้งานเป็นค่า Free
            }

            // วนลูปตรวจสอบ direct address ใน inode ทุกตัว
            for (int i = 0; i < NDIRECT; i++) {
                if (inode.addrs[i] != 0) {
                    // ตรวจสอบว่า direct address นั้นถูกใช้งานมาก่อนหรือยัง ถ้าใช้แล้วให้คืนค่า 1 
                    if (usedDirectAddresses[i] == 1) {
                        return 1; // Direct address used more than once
                    } 
                    //แต่ถ้ายังไม่ถูกใช้งานให้ Mark direct address นั้นให้มีค่าเท่ากับ 1
                    else {
                        usedDirectAddresses[i] = 1; // Mark the address as used
                    }
                }
            }
        }
        //ถ้าวนจนครบทุก inode ของ direct address แล้ว ให้ออกจาก for loop
    }
    //แล้วคืนค่า 0 กลับไปเพื่อบอกว่าไฟล์ fd ไม่มีปัญหาเรื่อง inode ที่ใช้งานอยู่ว่ามี data block ที่ชี้มายัง direct address มากกว่า 1 datablock
    return 0; // No errors found
}

//ตรวจสอบ inode ที่ใช้งานอยู่ว่ามี data block ที่ชี้มายัง indirect address มากกว่า 1 datablock หรือไม่ ถ้าเป็นจริง ให้แสดงข้อความ ERROR: indirect address used more than once.
int check_indirect_address_usage(int fd) {
    // วนลูปตรวจสอบ inode ทุกตัว
    for (int inum = 0; inum < sb.ninodes; inum++) {
        // ตรวจสอบว่า inode นั้นเป็นไฟล์ที่มี data block หรือเปล่า [Allocation File]
        if (readi(fd, inum, &inode) > 0) {
            // สร้างอาร์เรย์ไว้สำหรับเก็บ Track เพื่อใช้ในการเปรียบเทียบ
            int usedAddresses[NINDIRECT];
            for (int i = NDIRECT; i < NINDIRECT; i++) {
                usedIndirectAddresses[i] = 0; // ใส่ค่า 0 ลงไปตามขนาดของอาร์เรย์เพื่อใช้แทนค่าว่า inode ที่ตำแหน่งนี้ไม่มีการใช้งานเป็นค่า Free
            }

            //ตรวจสอบ indirect address ใน indirect datablock
            if (inode.addrs[NDIRECT] != 0) {
                // อ่านค่าจากไฟล์ fd, pointer* ชี้ให้เก็บไว้ใน buffer, ขนาดข้อมูลที่อ่านเท่ากับ BSIZE[521byte], offset เท่ากับ 6144
                if (pread(fd, buffer, BSIZE, inode.addrs[NDIRECT] * BSIZE) < 0) {
                    return 1; // Error reading the indirect block
                }

                // วนลูปตรวจสอบ indirect address ใน inode ทุกตัว
                for (int i = 0; i < NINDIRECT; i++) {
                    if (buffer[i] != 0) {
                        // ตรวจสอบว่า indirect address นั้นถูกใช้งานมาก่อนหรือยัง ถ้าใช้แล้วให้คืนค่า 1 
                        if (usedIndirectAddresses[i] == 1) {
                            return 1; // Indirect address used more than once
                        } 
                        //แต่ถ้ายังไม่ถูกใช้งานให้ Mark indirect address นั้นให้มีค่าเท่ากับ 1
                        else {
                            usedIndirectAddresses[i] = 1; // Mark the address as used
                        }
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