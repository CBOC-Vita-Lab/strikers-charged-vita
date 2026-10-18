# The SDK's MD5 reads the message words with byte-reversing loads (lwbrx) because the console is big-endian
# and MD5 is little-endian; on a little-endian host those are plain accesses.
s/NETReadSwappedBytes32(xp)/(*xp)/
s/NETSwapBytes32(\(.*\));/\1;/
s/NETWriteSwappedBytes32(\(p[^,]*\), \([^)]*\));/*(\1) = \2;/
