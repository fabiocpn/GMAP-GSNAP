/* $Id: indexdb.h,v 1.18 2005/02/14 13:01:09 twu Exp $ */
#ifndef INDEXDB_INCLUDED
#define INDEXDB_INCLUDED
#include <stdio.h>
#include "types.h"
#include "genomicpos.h"
#include "bool.h"
#include "iit-read.h"

#define INDEX1PART 12
#define INDEX1INTERVAL 6

/* Typically 12 nt or 24 bits, requiring 3 bytes */
typedef UINT4 Storedoligomer_T;

#define T Indexdb_T
typedef struct T *T;

extern void
Indexdb_free (T *old);
extern T
Indexdb_new_genome (char *genomesubdir, char *fileroot, bool batchp);
extern T
Indexdb_new_segment (char *genomicseg);

extern Genomicpos_T *
Indexdb_read (int *nentries, T this, Storedoligomer_T oligo);

extern void
Indexdb_write_offsets (FILE *offsets_fp, FILE *sequence_fp, IIT_T altstrain_iit,
		       bool uncompressedp);
extern void
Indexdb_write_positions (char *positionsfile, FILE *offsets_fp, FILE *sequence_fp, 
			 IIT_T altstrain_iit, bool uncompressedp, bool writefilep);

#undef T
#endif
