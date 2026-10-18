# NULL is 0 for MWCC; GCC 14+ refuses a pointer assigned to an integer field.
s/__AXDSPTask\.iramDspAddr = NULL;/__AXDSPTask.iramDspAddr = 0;/
