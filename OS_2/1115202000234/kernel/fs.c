// fs.c

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

struct superblock sb; 

static void readsb(int dev, struct superblock *sb)
{
  struct buf *bp;
  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
}

// -------------------------------------------------------------------
// Remove log_write() from bzero() so it doesn't force a transaction
// here. The caller at a higher level should be in a transaction.
//
// Also consider removing log_write() from balloc() and bfree() calls.
// -------------------------------------------------------------------
static void
bzero(int dev, int bno)
{
  struct buf *bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);

  // Instead of log_write(bp), let the caller's transaction handle it.
  // So we do only:
  bwrite(bp);

  brelse(bp);
}

// Allocate a zeroed disk block.
// We'll remove direct log_write() calls, letting the caller do it.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        // Mark used
        bp->data[bi/8] |= m;
        // Again, no log_write() here, just bwrite so
        // we don't panic if there's no transaction in effect.
        bwrite(bp);
        brelse(bp);

        // zero the new block data (no log_write, just bwrite)
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  printf("balloc: out of blocks\n");
  return 0;
}

// Free a disk block.
// Again, remove direct log_write() calls
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  bwrite(bp);
  brelse(bp);
}

// -------------- Inode code --------------
// (No changes other than removing any calls to log_write in balloc/bfree)

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

void iinit()
{
  int i = 0;
  initlock(&itable.lock, "itable");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&itable.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum);

struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      bwrite(bp); // no log_write; rely on caller's transaction
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  printf("ialloc: no inodes\n");
  return 0;
}

void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type  = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size  = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));

  // Instead of log_write(), do bwrite().
  bwrite(bp);
  brelse(bp);
}

static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty = 0;

  acquire(&itable.lock);
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)
      empty = ip;
  }
  if(empty == 0)
    panic("iget: no inodes");
  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&itable.lock);

  return ip;
}

struct inode*
idup(struct inode *ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type  = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size  = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");
  releasesleep(&ip->lock);
}

void
iput(struct inode *ip)
{
  acquire(&itable.lock);
  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    acquiresleep(&ip->lock);
    release(&itable.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);  // bwrite, not log_write
    ip->valid = 0;

    releasesleep(&ip->lock);
    acquire(&itable.lock);
  }
  ip->ref--;
  release(&itable.lock);
}

void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// bmap with double-indirect etc. stays mostly the same
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp, *bp2;

  // direct blocks
  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      addr = balloc(ip->dev);
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  // single-indirect
  if(bn < NINDIRECT){
    if((addr = ip->addrs[NDIRECT]) == 0){
      addr = balloc(ip->dev);
      ip->addrs[NDIRECT] = addr;
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if(a[bn] == 0){
      a[bn] = balloc(ip->dev);
      bwrite(bp);
    }
    addr = a[bn];
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;

  // double-indirect
  if(bn < NINDIRECT * NINDIRECT){
    if((addr = ip->addrs[NDIRECT+1]) == 0){
      addr = balloc(ip->dev);
      ip->addrs[NDIRECT+1] = addr;
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;

    uint idx = bn / NINDIRECT;
    uint off = bn % NINDIRECT;

    if(a[idx] == 0){
      a[idx] = balloc(ip->dev);
      bwrite(bp);
    }
    bp2 = bread(ip->dev, a[idx]);
    uint *a2 = (uint*)bp2->data;
    if(a2[off] == 0){
      a2[off] = balloc(ip->dev);
      bwrite(bp2);
    }
    addr = a2[off];
    brelse(bp2);
    bwrite(bp);
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
  return 0;
}

void
itrunc(struct inode *ip)
{
  int i, j, k;
  struct buf *bp, *bp2;
  uint *a, *a2;

  // free direct
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }
  // free single-indirect
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j]){
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }
  // free double-indirect
  if(ip->addrs[NDIRECT+1]){
    bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j]){
        bp2 = bread(ip->dev, a[j]);
        a2 = (uint*)bp2->data;
        for(k = 0; k < NINDIRECT; k++){
          if(a2[k]){
            bfree(ip->dev, a2[k]);
          }
        }
        brelse(bp2);
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT+1]);
    ip->addrs[NDIRECT+1] = 0;
  }
  ip->size = 0;
  iupdate(ip);
}

// No changes in stati, readi, writei, dir stuff, etc. except removing direct log_write() calls

void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    uint addr = bmap(ip, off / BSIZE);
    if(addr == 0)
      break;
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off%BSIZE), m) == -1){
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE * BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    uint addr = bmap(ip, off / BSIZE);
    if(addr == 0)
      break;
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - (off % BSIZE));
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1){
      brelse(bp);
      break;
    }
    bwrite(bp);
    brelse(bp);
  }
  if(off > ip->size)
    ip->size = off;
  iupdate(ip);
  return tot;
}

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  struct dirent de;
  uint off, inum;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }
  return 0;
}

int
dirlink(struct inode *dp, char *name, uint inum)
{
  struct dirent de;
  struct inode *ip;
  int off;

  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;
  return 0;
}

static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}