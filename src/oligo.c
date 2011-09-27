static char rcsid[] = "$Id: oligo.c,v 1.36 2005/02/15 01:58:50 twu Exp $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "oligo.h" 
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>		/* For toupper */
#include "mem.h"


#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif

/*               87654321 */
#define LEFT_A 0x00000000
#define LEFT_C 0x40000000
#define LEFT_G 0x80000000
#define LEFT_T 0xC0000000

/*                87654321 */
#define RIGHT_A 0x00000000
#define RIGHT_C 0x00000001
#define RIGHT_G 0x00000002
#define RIGHT_T 0x00000003

/************************************************************************
 *   Check
 ************************************************************************/

/* Note: positions is allocated by system malloc() */
int
Oligo_lookup (Genomicpos_T **positions, Indexdb_T indexdb, Storedoligomer_T storedoligo, 
	      cDNAEnd_T cdnaend, bool revcompp) {
  int nentries;
  
  debug(printf("Oligo on entry = %06X",storedoligo));
  
  if ((*positions = Indexdb_read(&nentries,indexdb,storedoligo)) == NULL) {
    debug(printf(" not found\n"));
    return 0;
    
  } else {
    debug(printf(" => %d entries\n",nentries));
    return nentries;
  }
}



static Oligostate_T
Oligo_read (int *querypos, Storedoligomer_T *forward, Storedoligomer_T *revcomp, 
	    Reader_T reader, cDNAEnd_T cdnaend) {
  int count = 0;
  int c;

  *forward = *revcomp = 0U;
  if (cdnaend == FIVE) {
    while (count < INDEX1PART && (c = Reader_getc(reader,cdnaend)) != '\0') {
      switch (toupper(c)) {
      case 'A': *forward <<= 2; *revcomp >>= 2; *revcomp |= LEFT_T; break;
      case 'C': *forward <<= 2; *forward |= RIGHT_C; 
	*revcomp >>= 2; *revcomp |= LEFT_G;  break;
      case 'G': *forward <<= 2; *forward |= RIGHT_G;
	*revcomp >>= 2; *revcomp |= LEFT_C; break;
      case 'T': *forward <<= 2; *forward |= RIGHT_T; *revcomp >>= 2; break;
      default: *forward = *revcomp = 0U; count = -1; 
	/* This counteracts count++ below */
      }
      count++;
      debug(printf("5' Read %c, count = %d, oligo = %06X, %06X\n",
		    c,count,*forward,*revcomp));
    }
  } else {
    while (count < INDEX1PART && (c = Reader_getc(reader,cdnaend)) != '\0') {
      switch (toupper(c)) {
      case 'A': *forward >>= 2; *revcomp <<= 2; *revcomp |= RIGHT_T; break;
      case 'C': *forward >>= 2; *forward |= LEFT_C; 
	*revcomp <<= 2; *revcomp |= RIGHT_G;  break;
      case 'G': *forward >>= 2; *forward |= LEFT_G;
	*revcomp <<= 2; *revcomp |= RIGHT_C; break;
      case 'T': *forward >>= 2; *forward |= LEFT_T; *revcomp <<= 2; break;
      default: *forward = *revcomp = 0U; count = -1; 
	/* This counteracts count++ below */
      }
      count++;
      debug(printf("3' Read %c, count = %d, oligo = %06X, %06X\n",
		    c,count,*forward,*revcomp));
    }
  }

  if (count < INDEX1PART) {
    *forward = *revcomp = 0U;
    return DONE;
  } else {
    debug(printf("Read: Returning oligo %06X for count = %d and size = %d\n",
		  *forward,count,INDEX1PART));
    if (cdnaend == FIVE) {
      *querypos = Reader_startpos(reader) - INDEX1PART;
      debug(printf("Setting querypos to be %u - %u = %u\n",
		   Reader_startpos(reader),INDEX1PART,*querypos));
    } else {
      *querypos = Reader_endpos(reader) + 1;
      debug(printf("Setting querypos to be %u + 1 = %u\n",
		   Reader_endpos(reader),*querypos));
    }
    return VALID;
  }
}

static Oligostate_T
Oligo_revise (int *querypos, Storedoligomer_T *forward, Storedoligomer_T *revcomp, 
	      Reader_T reader, cDNAEnd_T cdnaend) {
  char c;

  if ((c = Reader_getc(reader,cdnaend)) == '\0') {
    *forward = *revcomp = 0U;
    debug(
	  if (cdnaend == FIVE) {
	    printf("5' Revision: read terminating char %c\n",c);
	  } else {
	    printf("3' Revision: read terminating char %c\n",c);
	  }
	  );
    return DONE;
  } else if (cdnaend == FIVE) {
    switch (toupper(c)) {
    case 'A': *forward <<= 2; *revcomp >>= 2; *revcomp |= LEFT_T; break;
    case 'C': *forward <<= 2; *forward |= RIGHT_C; 
      *revcomp >>= 2; *revcomp |= LEFT_G; break;
    case 'G': *forward <<= 2; *forward |= RIGHT_G;
      *revcomp >>= 2; *revcomp |= LEFT_C; break;
    case 'T': *forward <<= 2; *forward |= RIGHT_T; *revcomp >>= 2; break;
    default: *forward = *revcomp = 0U; 
      *querypos = Reader_startpos(reader) - INDEX1PART;
      return INVALID;
    }

    *querypos = Reader_startpos(reader) - INDEX1PART;
    debug(printf("5' Revision: read char %c, oligo = %06X, %06X at querypos %d\n",
		  c,*forward,*revcomp,*querypos));
    return VALID;

  } else {
    switch (toupper(c)) {
    case 'A': *forward >>= 2; *revcomp <<= 2; *revcomp |= RIGHT_T; break;
    case 'C': *forward >>= 2; *forward |= LEFT_C; 
      *revcomp <<= 2; *revcomp |= RIGHT_G; break;
    case 'G': *forward >>= 2; *forward |= LEFT_G;
      *revcomp <<= 2; *revcomp |= RIGHT_C; break;
    case 'T': *forward >>= 2; *forward |= LEFT_T; *revcomp <<= 2; break;
    default: *forward = *revcomp = 0U; 
      *querypos = Reader_endpos(reader) + 1;
      return INVALID;
    }

    *querypos = Reader_endpos(reader) + 1;
    debug(printf("3' Revision: read char %c, oligo = %06X, %06X at querypos %d\n",
		  c,*forward,*revcomp,*querypos));
    return VALID;
  }
}


Oligostate_T
Oligo_next (Oligostate_T last_state, int *querypos, Storedoligomer_T *forward,
	    Storedoligomer_T *revcomp, Reader_T reader, cDNAEnd_T cdnaend) {

  if (last_state == DONE) {
    fprintf(stderr,"Called Oligo_next with last_state == DONE\n");
    exit(1);
  } else if (last_state == VALID) {
    return Oligo_revise(&(*querypos),&(*forward),&(*revcomp),reader,cdnaend);
  } else {			/* INVALID and INIT */
    return Oligo_read(&(*querypos),&(*forward),&(*revcomp),reader,cdnaend);
  }
}

Oligostate_T
Oligo_skip (Oligostate_T last_state, int *querypos, Storedoligomer_T *forward,
	    Storedoligomer_T *revcomp, Reader_T reader, cDNAEnd_T cdnaend, int nskip) {
  int i = 0;

  while (i < nskip && last_state != DONE) {
    if (last_state == VALID) {
      last_state = Oligo_revise(&(*querypos),&(*forward),&(*revcomp),reader,cdnaend);
      i++;
    } else {			/* INVALID and INIT */
      last_state = Oligo_read(&(*querypos),&(*forward),&(*revcomp),reader,cdnaend);
      i += INDEX1PART;
    }
  }
  return last_state;
}


/*                      87654321 */
#define LOW_TWO_BITS  0x00000003

/* Procedure used by oligo-count.c. */
char *
Oligo_nt (Storedoligomer_T oligo1, Storedoligomer_T oligo2, int oligosize) {
  char *nt;
  int i, j;
  Storedoligomer_T lowbits;

  nt = (char *) CALLOC(oligosize+1,sizeof(char));
  j = oligosize-1;
  for (i = 0; i < oligosize - INDEX1PART; i++) {
    lowbits = oligo2 & LOW_TWO_BITS;
    switch (lowbits) {
    case RIGHT_A: nt[j] = 'A'; break;
    case RIGHT_C: nt[j] = 'C'; break;
    case RIGHT_G: nt[j] = 'G'; break;
    case RIGHT_T: nt[j] = 'T'; break;
    }
    oligo2 >>= 2;
    j--;
  }

  for (i = 0; i < INDEX1PART; i++) {
    lowbits = oligo1 & LOW_TWO_BITS;
    switch (lowbits) {
    case RIGHT_A: nt[j] = 'A'; break;
    case RIGHT_C: nt[j] = 'C'; break;
    case RIGHT_G: nt[j] = 'G'; break;
    case RIGHT_T: nt[j] = 'T'; break;
    }
    oligo1 >>= 2;
    j--;
  }

  return nt;
}
