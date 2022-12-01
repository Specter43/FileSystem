echo "==========Let's Start!=========="

mkdir /tmp/mnt
truncate -s 64M img
./mkfs.a1fs -i 100 img
./a1fs img /tmp/mnt
echo "After mounting, the current state of the filesystem should look like the following"
echo ""
echo ""
ls -al /tmp/mnt
echo ""

echo "-------------Makes one directory--------------"
mkdir /tmp/mnt/dir
ls -al /tmp/mnt
echo "Remove it"
rmdir /tmp/mnt/dir
ls -al /tmp/mnt
echo ""

echo "-------------Make two directories that are direct children of root-------------"
mkdir /tmp/mnt/dir1
mkdir /tmp/mnt/dir2
ls -al /tmp/mnt
echo "Try to rename dir1 to dir2"
mv /tmp/mnt/dir1 /tmp/mnt/dir2
ls -al /tmp/mnt
echo "There should be a dir1 under dir2 now"
ls -al /tmp/mnt/dir2
echo "Clear disk"
rmdir /tmp/mnt/dir2/dir1
rmdir /tmp/mnt/dir2
echo ""

echo "-------------Make a bunch of nested directories-------------"
mkdir /tmp/mnt/a
mkdir /tmp/mnt/a/b
mkdir /tmp/mnt/a/b/c
mkdir /tmp/mnt/d
ls -al /tmp/mnt
echo "Try to rename c to d"
mv /tmp/mnt/a/b /tmp/mnt/d
echo "There should be 'b' under 'd' and  'c' under 'b'"
ls -al /tmp/mnt/d
ls -al /tmp/mnt/d/b
echo "'a' should be left alone"
ls /tmp/mnt/a
echo "Clear disk"
rmdir /tmp/mnt/a
rmdir /tmp/mnt/d/b/c
rmdir /tmp/mnt/d/b
rmdir /tmp/mnt/d
echo ""

echo "-------------Make one directory-------------"
mkdir /tmp/mnt/a
ls -al /tmp/mnt
echo "Try to rename 'a' to 'b' where 'b' does not exist"
mv /tmp/mnt/a /tmp/mnt/b
ls -al /tmp/mnt
echo "There should only be a 'b' now"
ls -al /tmp/mnt
echo "Clear disk"
rmdir /tmp/mnt/b
echo ""

echo "-------------Creates an empty file-------------"
touch /tmp/mnt/file1
echo "Checks the information about dir to make sure file1 is successfully created"
ls -al /tmp/mnt
echo ""

echo "-------------Write something to file1-------------"
echo "I love the CSC369!" > /tmp/mnt/file1
echo "The byte number of file1 should not be zero now"
ls -al /tmp/mnt
echo ""

echo "-------------Show the message in file1-------------"
cat /tmp/mnt/file1
echo ""

echo "-------------Show the bit-wise map of file1-------------"
xxd /tmp/mnt/file1
echo ""

echo "-------------Try to append file1 with other message-------------"
echo "!963CSC evol I" >> /tmp/mnt/file1
xxd /tmp/mnt/file1
echo ""

echo "-------------Unlinks file1-------------"
ls -al /tmp/mnt
unlink /tmp/mnt/file1
ls -al /tmp/mnt
echo ""

echo "-------------Create a new file and truncate it to the 4096B-------------"
touch /tmp/mnt/test
truncate -s 4096 /tmp/mnt/test
ls -al /tmp/mnt
echo ""

echo "-------------Use truncate to shrink the file to 100 bytes-------------"
truncate -s 100 /tmp/mnt/test
ls -al /tmp/mnt
echo ""

echo "-------------Unmount the file system and mount it back-------------"
fusermount -u /tmp/mnt
./a1fs img /tmp/mnt
ls -al /tmp/mnt

echo "===========The End==========="
