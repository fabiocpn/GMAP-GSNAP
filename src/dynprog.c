static char rcsid[] = "$Id: dynprog.c,v 1.111 2005/02/15 01:58:50 twu Exp $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "dynprog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>		/* For toupper */
#include <math.h>		/* For ceil, log, pow */
#include "bool.h"
#include "mem.h"
#include "pair.h"
#include "pairdef.h"
#include "boyer-moore.h"

/* Prints parameters and results */
#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif

/* Prints matrices */
#ifdef DEBUG2
#define debug2(x) x
#else
#define debug2(x)
#endif

/* Prints all bridge scores */
#ifdef DEBUG3
#define debug3(x) x
#else
#define debug3(x)
#endif

/*
#define RIGHTANGLE 1
*/

#define MICROEXON_PVALUE 0.01
#define ENDSEQUENCE_PVALUE 0.001 /* Have stricter threshold for making end exons */

#define MIN_MICROEXON_LENGTH 3
#define MAX_MICROEXON_LENGTH 12	/* Should be oligomer length - 1 plus peelback */
#define MICROINTRON_LENGTH 9

#define MIN_NONINTRON 50

#define FULLMATCH 3
#define INCOMPLETE 0
#define MISMATCH_HIGHQ -10
#define MISMATCH_MEDQ -9
#define MISMATCH_LOWQ -8

/* Allow lower mismatch scores on end to allow more complete
   alignments to the end, and because ends are typically of lower
   quality.  Make equal to FULLMATCH, because criterion is nmatches >=
   nmismatches. */
#define END_MISMATCH_HIGHQ -3
#define END_MISMATCH_MEDQ -3
#define END_MISMATCH_LOWQ -3

/* Note: In definitions below, extensions don't include the first base */

/* Make PAIRED the same as SINGLE.  Need to avoid the problem where
   gap-match-gap is preferred over two mismatches, so

   OPEN+FULLMATCH+OPEN < MISMATCH+MISMATCH, or

   OPEN < (MISMATCH+MISMATCH-FULLMATCH)/2

   In middle:
            (mismatch+mismatch-fullmatch)/2   open
   HIGHQ     (-20-3)/2=-11.5                  -17
   MEDQ      (-18-3)/2=-10.5                  -15
   LOWQ      (-16-3)/2= -9.5                  -13

   At ends:
            (mismatch+mismatch-fullmatch)/2   open
   HIGHQ     (-14-3)/2= -8.5                  -15
   MEDQ      (-12-3)/2= -7.5                  -13
   LOWQ      (-10-3)/2= -6.5                  -11 
*/

#define PAIRED_OPEN_HIGHQ -17
#define PAIRED_OPEN_MEDQ -15
#define PAIRED_OPEN_LOWQ -13

#define PAIRED_EXTEND_HIGHQ -7
#define PAIRED_EXTEND_MEDQ -6
#define PAIRED_EXTEND_LOWQ -5

#define SINGLE_OPEN_HIGHQ -17
#define SINGLE_OPEN_MEDQ -15
#define SINGLE_OPEN_LOWQ -13

#define SINGLE_EXTEND_HIGHQ -7
#define SINGLE_EXTEND_MEDQ -6
#define SINGLE_EXTEND_LOWQ -5


/* cDNA insertions are biologically not meaningful, so look for a good
   gap opening somewhere */
#define CDNA_OPEN_HIGHQ -10
#define CDNA_OPEN_MEDQ -9
#define CDNA_OPEN_LOWQ -8

#define CDNA_EXTEND_HIGHQ -7
#define CDNA_EXTEND_MEDQ -6
#define CDNA_EXTEND_LOWQ -5

/* Ends tend to be of lower quality, so we drop the scores a notch */
#define END_OPEN_HIGHQ -9
#define END_OPEN_MEDQ -7
#define END_OPEN_LOWQ -5

#define END_EXTEND_HIGHQ -6
#define END_EXTEND_MEDQ -5
#define END_EXTEND_LOWQ -4


/* To reward one mismatch, but not two, should make

   FULLMATCH < INTRON+MISMATCH, and
   FULLMATCH+FULLMATCH > INTRON+MISMATCH+MISMATCH, or

   FULLMATCH-MISMATCH < INTRON < FULLMATCH+FULLMATCH-MISMATCH-MISMATCH

             1 mismatch    2 mismatches  3 mismatches  intron
   HIGHQ     3-(-10)=13 ** 6-(-20)=26    9-(-30)=39      22
   MEDQ      3-(-9)= 12    6-(-18)=24 ** 9-(-27)=36      25
   LOWQ      3-(-8)= 11    6-(-16)=22 ** 9-(-24)=33      28 */

/* To reward one gap, but not two, in preference to matching part of
   the dinucleotide, 

   FULLMATCH < INTRON+OPEN, and
   FULLMATCH < INTRON+OPEN+EXTEND, or

   FULLMATCH-OPEN < INTRON < FULLMATCH-OPEN-EXTEND

             1 gap         gap+extend    gap+2extend   intron
   HIGHQ     3-(-17)=20 ** 3-(-24)=26    3-(-31)=34      22
   MEDQ      3-(-15)=18    3-(-21)=24 ** 3-(-27)=30      25
   LOWQ      3-(-13)=16    3-(-18)=21    3-(-23)=26 **   28 */

#define CANONICAL_INTRON_HIGHQ 28 /* GT-AG */
#define CANONICAL_INTRON_MEDQ  31
#define CANONICAL_INTRON_LOWQ  34

/* Prefer alternate intron to other non-canonicals, but don't
   introduce mismatches or gaps to identify */
#define ALTERNATE_INTRON 10	/* GC-AG or AT-AC */

/* .01 = Prob(noncanonical) > Prob(sequence gap) = 0.003*(.20) */
#define MAXHORIZJUMP_HIGHQ 1
#define MAXVERTJUMP_HIGHQ 1

/* .01 = Prob(noncanonical) > Prob(sequence gap) = 0.014*(.20) */
#define MAXHORIZJUMP_MEDQ 1
#define MAXVERTJUMP_MEDQ 1

/* .01 = Prob(noncanonical) < Prob(sequence gap) */
#define MAXHORIZJUMP_LOWQ 1
#define MAXVERTJUMP_LOWQ 1


typedef char Direction_T;
#define VERT 4
#define HORIZ 2
#define DIAG 1
#define STOP 0


/************************************************************************
 * Matrix
 ************************************************************************/

/* Makes a matrix of dimensions 0..length1 x 0..length2 inclusive */
static int **
Matrix_alloc (int length1, int length2, int **ptrs, int *space, 
	      int lband, int rband) {
  int **matrix;
  int r, clo, chigh, i;

  matrix = ptrs;
  matrix[0] = space;
  for (i = 1; i <= length1; i++) {
    matrix[i] = matrix[i-1] + (length2 + 1);
  }

  /* Clear memory only around the band, but doesn't work with Gotoh P1
     and Q1 matrices */
  /*
  for (r = 0; r <= length1; r++) {
    if ((clo = r - lband - 1) >= 0) {
      matrix[r][clo] = 0;
    }
    if ((chigh = r + rband + 1) <= length2) {
      matrix[r][chigh] = 0;
    }
  }
  */
  memset(space,0,(length1+1)*(length2+1)*sizeof(int));

  return matrix;
}

static void
Matrix_print (int **matrix, int length1, int length2, char *sequence1, char *sequence2,
	      bool revp) {
  int i, j;

  printf("  ");
  for (j = 0; j <= length2; ++j) {
    if (j == 0) {
      printf("    ");
    } else {
      printf("  %c ",revp ? sequence2[-j+1] : sequence2[j-1]);
    }
  }
  printf("\n");

  for (i = 0; i <= length1; ++i) {
    if (i == 0) {
      printf("  ");
    } else {
      printf("%c ",revp ? sequence1[-i+1] : sequence1[i-1]);
    }
    for (j = 0; j <= length2; ++j) {
      printf("%3d ",matrix[i][j]);
    }
    printf("\n");
  }
  printf("\n");
  return;
}

/************************************************************************/
/*  Directions  */
/************************************************************************/

/* Makes a matrix of dimensions 0..length1 x 0..length2 inclusive */
static Direction_T **
Directions_alloc (int length1, int length2, Direction_T **ptrs, Direction_T *space,
		  int lband, int rband) {
  Direction_T **directions;
  int r, clo, chigh, i;

  directions = ptrs;
  directions[0] = space;
  for (i = 1; i <= length1; i++) {
    directions[i] = directions[i-1] + (length2 + 1);
  }

  /* Clear memory only around the band, but may not work with Gotoh
     method */
  /*
  for (r = 0; r <= length1; r++) {
    if ((clo = r - lband - 1) >= 0) {
      directions[r][clo] = STOP;
    }
    if ((chigh = r + rband + 1) <= length2) {
      directions[r][chigh] = STOP;
    }
  }
  */
  memset(space,0,(length1+1)*(length2+1)*sizeof(Direction_T));

  return directions;
}

static void
Directions_print (Direction_T **directions, int **jump, int length1, int length2, 
		  char *sequence1, char *sequence2, bool revp) {
  int i, j;
  char buffer[4];

  printf("  ");
  for (j = 0; j <= length2; ++j) {
    if (j == 0) {
      printf("    ");
    } else {
      printf("  %c ",revp ? sequence2[-j+1] : sequence2[j-1]);
    }
  }
  printf("\n");

  for (i = 0; i <= length1; ++i) {
    if (i == 0) {
      printf("  ");
    } else {
      printf("%c ",revp ? sequence1[-i+1] : sequence1[i-1]);
    }
    for (j = 0; j <= length2; ++j) {
      if (directions[i][j] == DIAG) {
	sprintf(buffer,"D%d",jump[i][j]);
      } else if (directions[i][j] == HORIZ) {
	sprintf(buffer,"H%d",jump[i][j]);
      } else if (directions[i][j] == VERT) {
	sprintf(buffer,"V%d",jump[i][j]);
      } else {
	sprintf(buffer,"S%d",0);
      }
      printf("%3s ",buffer);
    }
    printf("\n");
  }
  printf("\n");
  return;
}




#define T Dynprog_T
struct T {
  int maxlength1;
  int maxlength2;

  int **matrix_ptrs, *matrix_space;
  Direction_T **directions_ptrs, *directions_space;
  int **jump_ptrs, *jump_space;
};


T
Dynprog_new (int maxlookback, int extraquerygap, int maxpeelback,
	     int extramaterial_end, int extramaterial_paired) {
  T new = (T) MALLOC(sizeof(*new));
  int maxlength1, maxlength2;

  new->maxlength1 = maxlookback + maxpeelback;
  new->maxlength2 = new->maxlength1 + extraquerygap;
  if (extramaterial_end > extramaterial_paired) {
    new->maxlength2 += extramaterial_end;
  } else {
    new->maxlength2 += extramaterial_paired;
  }
  maxlength1 = new->maxlength1;
  maxlength2 = new->maxlength2;

  new->matrix_ptrs = (int **) CALLOC(maxlength1+1,sizeof(int *));
  new->matrix_space = (int *) CALLOC((maxlength1+1)*(maxlength2+1),sizeof(int));
  new->directions_ptrs = (Direction_T **) CALLOC(maxlength1+1,sizeof(Direction_T *));
  new->directions_space = (Direction_T *) CALLOC((maxlength1+1)*(maxlength2+1),sizeof(Direction_T));
  new->jump_ptrs = (int **) CALLOC(maxlength1+1,sizeof(int *));
  new->jump_space = (int *) CALLOC((maxlength1+1)*(maxlength2+1),sizeof(int));

  return new;
}


void
Dynprog_free (T *old) {
  if (*old) {
    FREE((*old)->matrix_ptrs);
    FREE((*old)->matrix_space);
    FREE((*old)->directions_ptrs);
    FREE((*old)->directions_space);
    FREE((*old)->jump_ptrs);
    FREE((*old)->jump_space);

    FREE(*old);
  }
  return;
}

/************************************************************************/


static int
pairdistance (char *sequence1, char *sequence2, bool revp,
	      int index1, int index2, int mismatch) {
  int na1, na2;

  /* We assume that the sequences have been converted to upper case */
  if (revp) {
    na1 = toupper(sequence1[-index1]);
    na2 = toupper(sequence2[-index2]);
  } else {
    na1 = toupper(sequence1[index1]);
    na2 = toupper(sequence2[index2]);
  }
  if (na1 == na2) {
    return FULLMATCH;
  } else if (na1 == 'N' || na1 == 'X') {
    return INCOMPLETE;
  } else if (na2 == 'N' || na2 == 'X') {
    return INCOMPLETE;
  } else {
    return mismatch;
  }
}

static int **
compute_scores_affine (Direction_T ***directions, int ***jump, T this, 
		       char *sequence1, char *sequence2, bool revp, 
		       int length1, int length2, 
		       int mismatch, int open, int extend, 
		       int extraband) {
  int **matrix;
  int r, c, r1, c1, pen;
  int bestscore, score, bestjump, j;
  Direction_T bestdir;
  int scoreV, scoreH, scoreD;
  int lband, rband, cmid, clo, chigh, rlo;
  bool endp = false;

  if (length2 >= length1) {
    rband = length2 - length1 + extraband;
    lband = extraband;
  }

  matrix = Matrix_alloc(length1,length2,this->matrix_ptrs,this->matrix_space,lband,rband);
  *directions = Directions_alloc(length1,length2,this->directions_ptrs,this->directions_space,lband,rband);
  *jump = Matrix_alloc(length1,length2,this->jump_ptrs,this->jump_space,lband,rband);

  matrix[0][0] = 0;
  (*directions)[0][0] = STOP;
  (*jump)[0][0] = 0;

  /* Row 0 initialization */
  pen = open;
  for (c = 1; c <= rband && c <= length2; c++) {
    matrix[0][c] = pen;
    (*directions)[0][c] = HORIZ;
    (*jump)[0][c] = c;
    pen += extend;
  }

  /* Column 0 initialization */
  pen = open;
  for (r = 1; r <= lband && r <= length1; r++) {
    matrix[r][0] = pen;
    (*directions)[r][0] = VERT;
    (*jump)[r][0] = r;
    pen += extend;
  }

  for (r = 1; r <= length1; r++) {
    if ((clo = r - lband) < 1) {
      clo = 1;
    }
    if ((chigh = r + rband) > length2) {
      chigh = length2;
    }
    for (c = clo; c <= chigh; c++) {
      /* Diagonal case */
      bestscore = matrix[r-1][c-1] + pairdistance(sequence1,sequence2,revp,r-1,c-1,mismatch);
      bestdir = DIAG;
      bestjump = 1;
      
      /* Horizontal case */
      pen = open;
      for (c1 = c-1, j = 1; c1 >= clo; c1--, j++) {
	if ((*directions)[r][c1] == DIAG) {
	  score = matrix[r][c1] + pen;
	  if (score > bestscore) {
	    bestscore = score;
	    bestdir = HORIZ;
	    bestjump = j;
	  }
#ifdef RIGHTANGLE	  
	} else if ((*directions)[r][c1] == VERT) {
	  score = matrix[r][c1] + pen - open; /* Compensate for double opening */
	  if (score > bestscore) {
	    bestscore = score;
	    bestdir = HORIZ;
	    bestjump = j;
	  }
#endif
	}
	pen += extend;
      }

      /* Vertical case */
      if ((rlo = c+c-rband-r) < 1) {
	rlo = 1;
      }
      pen = open;
      for (r1 = r-1, j = 1; r1 >= rlo; r1--, j++) {
	if ((*directions)[r1][c] == DIAG) {
	  score = matrix[r1][c] + pen;
	  if (score > bestscore) {
	    bestscore = score;
	    bestdir = VERT;
	    bestjump = j;
	  }
#ifdef RIGHTANGLE
	} else if ((*directions)[r1][c] == HORIZ) {
	  score = matrix[r1][c] + pen - open; /* Compensate for double opening */
	  if (score > bestscore) {
	    bestscore = score;
	    bestdir = VERT;
	    bestjump = j;
	  }
#endif
	}
	pen += extend;
      }

      /*
	debug(printf("At %d,%d, scoreV = %d, scoreH = %d, scoreD = %d\n",
	r,c,scoreV,scoreH,scoreD));
      */
      
      /* Update */
      matrix[r][c] = bestscore;
      (*directions)[r][c] = bestdir;
      (*jump)[r][c] = bestjump;
    }
  }

  /*
  debug2(Matrix_print(P0,length1,length2));
  debug2(Matrix_print(P1,length1,length2));
  debug2(Matrix_print(Q0,length1,length2));
  debug2(Matrix_print(Q1,length1,length2));
  */
  debug2(Matrix_print(matrix,length1,length2,sequence1,sequence2,revp));
  debug2(Directions_print(*directions,*jump,length1,length2,
			  sequence1,sequence2,revp));

  return matrix;
}

static int
jump_penalty_affine (int length, int open, int extend) {
  return open + (length - 1)*extend;
}

/* For lengths of 1,2,3, returns open.  Then, for lengths of 4,5,6,
   returns open + 3*extend.  Add extra extend to penalty, so ---- is
   preferred over ---|-. */
static int
jump_penalty (int length, int open, int extend) {
  int ncodons;

  ncodons = (length - 1)/3;
  return open + extend + ncodons*3*extend;
}


static int **
compute_scores (Direction_T ***directions, int ***jump, T this, 
		char *sequence1, char *sequence2, bool revp, 
		int length1, int length2, 
		int mismatch, int open, int extend, int extraband) {
  int **matrix;
  int r, c, r1, c1;
  int bestscore, score, bestjump, j;
  Direction_T bestdir;
  int scoreV, scoreH, scoreD;
  int lband, rband, cmid, clo, chigh, rlo;

  if (length2 >= length1) {
    rband = length2 - length1 + extraband;
    lband = extraband;
  } else {
    lband = length1 - length2 + extraband;
    rband = extraband;
  }

  matrix = Matrix_alloc(length1,length2,this->matrix_ptrs,this->matrix_space,lband,rband);
  *directions = Directions_alloc(length1,length2,this->directions_ptrs,this->directions_space,lband,rband);
  *jump = Matrix_alloc(length1,length2,this->jump_ptrs,this->jump_space,lband,rband);

  matrix[0][0] = 0;
  (*directions)[0][0] = STOP;
  (*jump)[0][0] = 0;

  /* Row 0 initialization */
  for (c = 1; c <= rband && c <= length2; c++) {
    matrix[0][c] = jump_penalty(c,open,extend);
    (*directions)[0][c] = HORIZ;
    (*jump)[0][c] = c;
  }

  /* Column 0 initialization */
  for (r = 1; r <= lband && r <= length1; r++) {
    matrix[r][0] = jump_penalty(r,open,extend);
    (*directions)[r][0] = VERT;
    (*jump)[r][0] = r;
  }

  for (r = 1; r <= length1; r++) {
    if ((clo = r - lband) < 1) {
      clo = 1;
    }
    if ((chigh = r + rband) > length2) {
      chigh = length2;
    }
    for (c = clo; c <= chigh; c++) {
      /* Diagonal case */
      bestscore = matrix[r-1][c-1] + pairdistance(sequence1,sequence2,revp,r-1,c-1,mismatch);
      bestdir = DIAG;
      bestjump = 1;
      
      /* Horizontal case */
      for (c1 = c-1, j = 1; c1 >= clo; c1--, j++) {
	if ((*directions)[r][c1] == DIAG) {
	  score = matrix[r][c1] + jump_penalty(j,open,extend);
	  if (score > bestscore) {
	    bestscore = score;
	    bestdir = HORIZ;
	    bestjump = j;
	  }
#ifdef RIGHTANGLE
	} else if ((*directions)[r][c1] == VERT && (*jump)[r][c1] >= 3) {
	  score = matrix[r][c1] + jump_penalty(j,open,extend) - open;
	  if (score > bestscore) {
	    bestscore = score;
	    bestdir = HORIZ;
	    bestjump = j;
	  }
#endif
	}
      }

      /* Vertical case */
      if ((rlo = c+c-rband-r) < 1) {
	rlo = 1;
      }
      for (r1 = r-1, j = 1; r1 >= rlo; r1--, j++) {
	if ((*directions)[r1][c] == DIAG) {
	  score = matrix[r1][c] + jump_penalty(j,open,extend);
	  if (score > bestscore) {
	    bestscore = score;
	    bestdir = VERT;
	    bestjump = j;
	  }
#ifdef RIGHTANGLE
	} else if ((*directions)[r1][c] == HORIZ && (*jump)[r1][c] >= 3) {
	  score = matrix[r1][c] + jump_penalty(j,open,extend) - open;
	  if (score > bestscore) {
	    bestscore = score;
	    bestdir = VERT;
	    bestjump = j;
	  }
#endif
	}
      }

      /*
	debug(printf("At %d,%d, scoreV = %d, scoreH = %d, scoreD = %d\n",
	r,c,scoreV,scoreH,scoreD));
      */
      
      /* Update */
      matrix[r][c] = bestscore;
      (*directions)[r][c] = bestdir;
      (*jump)[r][c] = bestjump;
    }
  }

  /*
  debug2(Matrix_print(P0,length1,length2));
  debug2(Matrix_print(P1,length1,length2));
  debug2(Matrix_print(Q0,length1,length2));
  debug2(Matrix_print(Q1,length1,length2));
  */
  debug2(Matrix_print(matrix,length1,length2,sequence1,sequence2,revp));
  debug2(Directions_print(*directions,*jump,length1,length2,
			  sequence1,sequence2,revp));

  return matrix;
}

#if 0
static void
find_best_endpoint (int *finalscore, int *bestr, int *bestc, int **matrix, 
		    int length1, int length2, int extraband_end_or_paired) {
  int bestscore = 0;
  int r, c;
  int rband, lband, clo, chigh;
  /* No need for loffset or cmid because they apply only for cdnaend
     == FIVE, which doesn't require searching */

  *bestr = *bestc = 0;

  rband = length2 - length1 + extraband_end_or_paired;
  lband = extraband_end_or_paired;

  for (r = 1; r <= length1; r++) {
    if ((clo = r - lband) < 1) {
      clo = 1;
    }
    if ((chigh = r + rband) > length2) {
      chigh = length2;
    }
    for (c = clo; c <= chigh; c++) {
      if (matrix[r][c] > bestscore) {
	*bestr = r;
	*bestc = c;
	bestscore = matrix[r][c];
      }
    }
  }
  *finalscore = bestscore;
  return;
}
#endif

static void
find_best_endpoint_onegap (int *finalscore, int *bestr, int *bestc, int **matrix, 
			   int length1, int length2, int extraband_end,
			   bool extend_mismatch_p) {
  int bestscore = -1000000;
  int r, c;
  int rband, lband, clo, chigh;
  /* No need for loffset or cmid because they apply only for cdnaend
     == FIVE, which doesn't require searching */

  *bestr = *bestc = 0;

  rband = lband = 1;

  for (r = 1; r <= length1; r++) {
    if ((clo = r - lband) < 1) {
      clo = 1;
    }
    if ((chigh = r + rband) > length2) {
      chigh = length2;
    }
    for (c = clo; c <= chigh; c++) {
      if (matrix[r][c] >= bestscore) {
	*bestr = r;
	*bestc = c;
	bestscore = matrix[r][c];
      }
    }
  }

  *finalscore = bestscore;

  if (extend_mismatch_p == true) {
    if (*bestr == length1 - 1) {
      if ((chigh = *bestr + rband) > length2) {
	chigh = length2;
      }
      if (*bestc < chigh) {
	debug(printf("Extending by 1 just to be complete\n"));
	*bestr += 1;
	*bestc += 1;
      }
    }
  }
  return;
}


#if 0
/* Finds best score along diagonal.  Not used anymore. */
static void
find_best_endpoint_nogap (int *finalscore, int *bestr, int *bestc, int **matrix, 
			  int length1, int length2, int extraband_end_or_paired) {
  int bestscore = -1000000;
  int r, c;
  /* No need for loffset or cmid because they apply only for cdnaend
     == FIVE, which doesn't require searching */

  *bestr = *bestc = 0;

  for (r = 1; r <= length1; r++) {
    c = r;
    if (matrix[r][c] >= bestscore) {
      *bestr = r;
      *bestc = c;
      bestscore = matrix[r][c];
    }
  }
  *finalscore = bestscore;
  return;
}
#endif



/************************************************************************
 *  Scheme for matching dinucleotide pairs
 ************************************************************************/

/* Pieces for logical AND */
#define LEFT_GT  0x21		/* 100001 */
#define LEFT_GC	 0x10		/* 010000 */
#define LEFT_AT  0x08		/* 001000 */
#define LEFT_CT  0x06		/* 000110 */

#define RIGHT_AG 0x30		/* 110000 */
#define RIGHT_AC 0x0C		/* 001100 */
#define RIGHT_GC 0x02		/* 000010 */
#define RIGHT_AT 0x01		/* 000001 */


static List_T
add_gap (List_T pairs, int bestr, int bestcL, int bestcR,
	 char *sequence1, char *sequence2L, char *revsequence2R,
	 int length1, int length2L, int length2R, 
	 int offset1, int offset2L, int revoffset2R, 
	 int introntype, int ngap, Pairpool_T pairpool) {
  char comp, c2;
  int intronlength;
  int i;

  switch (introntype) {
  case GTAG_FWD: comp = '>'; break;
  case GCAG_FWD: comp = ')'; break;
  case ATAC_FWD: comp = ']'; break;
  case NONINTRON:
    intronlength = (revoffset2R-bestcR) - (offset2L+bestcL) + 1;
    if (intronlength < MIN_NONINTRON) {
      comp = '~';		/* Will be printed as '-', but need to score as '=' */
    } else {
      comp = '=';
    }
    break;
  case ATAC_REV: comp = '['; break;
  case GCAG_REV: comp = '('; break;
  case GTAG_REV: comp = '<'; break;
  default: fprintf(stderr,"Unexpected intron type %d\n",introntype);
    exit(9);
  }
  debug(printf("Adding gap of type %c\n",comp));

  if (comp == '~') {
    /* Treat as an insertion rather than an intron */
    for (i = 0; i < intronlength; i++) {
      /* c2 = (bestcR+i < length2R) ? revsequence2R[-bestcR-i] : '?'; */
      c2 = revsequence2R[-bestcR-i];
      pairs = Pairpool_push(pairs,pairpool,offset1+bestr,revoffset2R-(bestcR+i),' ',comp,c2);
    }
  } else {
    for (i = 0; i < ngap; i++) {
      /* c2 = (bestcR+i < length2R) ? revsequence2R[-bestcR-i] : '?'; */
      c2 = revsequence2R[-bestcR-i];
      pairs = Pairpool_push(pairs,pairpool,offset1+bestr,revoffset2R-(bestcR+i),' ',comp,c2);
    }
    
    pairs = Pairpool_push(pairs,pairpool,offset1+bestr,revoffset2R-bestcR-ngap+1,' ','.','.');
    pairs = Pairpool_push(pairs,pairpool,offset1+bestr,revoffset2R-bestcR-ngap+1,' ','.','.');
    pairs = Pairpool_push(pairs,pairpool,offset1+bestr,revoffset2R-bestcR-ngap+1,' ','.','.');
    
    for (i = ngap-1; i >= 0; i--) {
      /* c2 = (bestcL+i < length2L) ? sequence2L[bestcL+i] : '?'; */
      c2 = sequence2L[bestcL+i];
      pairs = Pairpool_push(pairs,pairpool,offset1+bestr,offset2L+bestcL+i,' ',comp,c2);
    }
  }

  return pairs;
}


/* Preferences: Continue in same direction if possible.  This has the
   effect of extending gaps to maximum size. Then, take diagonal if
   possible. Finally, take vertical if possible, because this will use
   up sequence 1, which is the query (cDNA) sequence. */

/* revp means both rev1p and rev2p, which must have equal values */
/* Iterative version */
static List_T
traceback (List_T pairs, int *nmatches, int *nmismatches, int *nopens, int *nindels,
	   Direction_T **directions, int **jump, int r, int c, 
	   char *sequence1, char *sequence2, int offset1, int offset2, 
	   Pairpool_T pairpool, bool revp, int ngap, int cdna_direction) {
  char c1, c2;
  char match = '*';
  int dist, j;
  char left1, left2, right2, right1, comp;
  int introntype, leftdi, rightdi;
  bool add_dashes_p;

  while (directions[r][c] != STOP) {
    if (directions[r][c] == DIAG) {
      c1 = revp ? sequence1[-r+1] : sequence1[r-1];
      c2 = revp ? sequence2[-c+1] : sequence2[c-1];
      debug(printf("D%d: Pushing %d,%d (%c,%c)\n",jump[r][c],r,c,c1,c2));
      if (toupper(c1) == toupper(c2)) {
	*nmatches += 1;
	if (revp) {
	  pairs = Pairpool_push(pairs,pairpool,offset1-(r-1),offset2-(c-1),c1,match,c2);
	} else {
	  pairs = Pairpool_push(pairs,pairpool,offset1+r-1,offset2+c-1,c1,match,c2);
	}
      } else {
	*nmismatches += 1;
	if (revp) {
	  pairs = Pairpool_push(pairs,pairpool,offset1-(r-1),offset2-(c-1),c1,' ',c2);
	} else {
	  pairs = Pairpool_push(pairs,pairpool,offset1+r-1,offset2+c-1,c1,' ',c2);
	}
      }
      r--; c--;
    } else if (directions[r][c] == HORIZ) {
      debug(printf("H%d: ",jump[r][c]));
      if (ngap == 0 || (dist = jump[r][c]) < MICROINTRON_LENGTH) {
	add_dashes_p = true;
      } else {
	/* Check for intron */
	if (revp == 1) {
	  left1 = sequence2[-c+1];
	  left2 = sequence2[-(c-1)+1];
	  right2 = sequence2[-(c-(dist-2))+1];
	  right1 = sequence2[-(c-(dist-1))+1];

	} else {
	  left1 = sequence2[(c-(dist-1))-1];
	  left2 = sequence2[(c-(dist-2))-1];
	  right2 = sequence2[(c-1)-1];
	  right1 = sequence2[c-1];
	}
	
	if (left1 == 'G' && left2 == 'T') {
	  leftdi = LEFT_GT;
	} else if (left1 == 'G' && left2 == 'C') {
	  leftdi = LEFT_GC;
	} else if (left1 == 'A' && left2 == 'T') {
	  leftdi = LEFT_AT;
	} else if (left1 == 'C' && left2 == 'T') {
	  leftdi = LEFT_CT;
	} else {
	  leftdi = 0x00;
	}

	if (right2 == 'A' && right1 == 'G') {
	  rightdi = RIGHT_AG;
	} else if (right2 == 'A' && right1 == 'C') {
	  rightdi = RIGHT_AC;
	} else if (right2 == 'G' && right1 == 'C') {
	  rightdi = RIGHT_GC;
	} else if (right2 == 'A' && right1 == 'T') {
	  rightdi = RIGHT_AT;
	} else {
	  rightdi = 0x00;
	}

	introntype = leftdi & rightdi;
	if (cdna_direction > 0) {
	  switch (introntype) {
	  case GTAG_FWD: comp = '>'; break;
	  case GCAG_FWD: comp = ')'; break;
	  case ATAC_FWD: comp = ']'; break;
	  default: comp = '=';
	  }
	} else if (cdna_direction < 0) {
	  switch(introntype) {
	  case ATAC_REV: comp = '['; break;
	  case GCAG_REV: comp = '('; break;
	  case GTAG_REV: comp = '<'; break;
	  default: comp = '=';
	  }
	} else {
	  comp = '=';
	}

	if (comp == '=') {
	  add_dashes_p = true;
	} else {
	  add_dashes_p = false;
	}
      }

      if (add_dashes_p == true) {
	/* Add dashes */
	*nopens += 1;
	*nindels += (dist = jump[r][c]);
	for (j = 0; j < dist; j++) {
	  c2 = revp ? sequence2[-c+1] : sequence2[c-1];
	  debug(printf("Pushing %d,%d (-,%c),",r,c,c2));
	  if (revp) {
	    pairs = Pairpool_push(pairs,pairpool,offset1-(r-1),offset2-(c-1),' ','-',c2);
	  } else {
	    pairs = Pairpool_push(pairs,pairpool,offset1+r-1,offset2+c-1,' ','-',c2);
	  }
	  c--;
	}
      } else {
	debug(printf("Large gap %c%c..%c%c.  Adding gap of type %c.\n",
		     left1,left2,right2,right1,comp));

	if (revp) {
	  for (j = 0; j < ngap; j++) {
	    c2 = sequence2[-(c-j)+1];
	    pairs = Pairpool_push(pairs,pairpool,offset1-(r-1),offset2-(c-j-1),
				  ' ',comp,c2);
	  }
	  pairs = Pairpool_push(pairs,pairpool,offset1-(r-1),offset2-(c-ngap-1),
				' ','.','.');
	  pairs = Pairpool_push(pairs,pairpool,offset1-(r-1),offset2-(c-ngap-1),
				' ','.','.');
	  pairs = Pairpool_push(pairs,pairpool,offset1-(r-1),offset2-(c-ngap-1),
				' ','.','.');

	  c -= dist - 1;
	  for (j = ngap-1; j >= 0; j--) {
	    c2 = sequence2[-(c+j)+1];
	    pairs = Pairpool_push(pairs,pairpool,offset1-(r-1),offset2-(c+j-1),
				  ' ',comp,c2);
	  }
	  c--;

	} else {
	  for (j = 0; j < ngap; j++) {
	    c2 = sequence2[c-j-1];
	    pairs = Pairpool_push(pairs,pairpool,offset1+(r-1),offset2+(c-j-1),
				  ' ',comp,c2);
	  }
	  pairs = Pairpool_push(pairs,pairpool,offset1+(r-1),offset2+(c-ngap-1),
				' ','.','.');
	  pairs = Pairpool_push(pairs,pairpool,offset1+(r-1),offset2+(c-ngap-1),
				' ','.','.');
	  pairs = Pairpool_push(pairs,pairpool,offset1+(r-1),offset2+(c-ngap-1),
				' ','.','.');

	  c -= dist - 1;
	  for (j = ngap-1; j >= 0; j--) {
	    c2 = sequence2[c+j-1];
	    pairs = Pairpool_push(pairs,pairpool,offset1+(r-1),offset2+(c+j-1),
				  ' ',comp,c2);
	  }
	  c--;

	}
      }

      debug(printf("\n"));

    } else if (directions[r][c] == VERT) {
      debug(printf("V%d: ",jump[r][c]));
      *nopens += 1;
      *nindels += (dist = jump[r][c]);
      for (j = 0; j < dist; j++) {
	c1 = revp ? sequence1[-r+1] : sequence1[r-1];
	debug(printf("Pushing %d,%d (%c,-), ",r,c,c1));
	if (revp) {
	  pairs = Pairpool_push(pairs,pairpool,offset1-(r-1),offset2-(c-1),c1,'-',' ');
	} else {
	  pairs = Pairpool_push(pairs,pairpool,offset1+r-1,offset2+c-1,c1,'-',' ');
	}
	r--;
      }
      debug(printf("\n"));
    } else {
      abort();
    }
  }
  return pairs;
}


static void
countback (int *nmatches, int *nmismatches, int *nopens, int *nindels,
	   Direction_T **directions, int **jump, int r, int c, 
	   char *sequence1, char *sequence2, int offset1, int offset2, 
	   Pairpool_T pairpool, bool revp) {
  char c1, c2;
  char match = '*';
  int dist, j;

  while (directions[r][c] != STOP) {
    if (directions[r][c] == DIAG) {
      c1 = revp ? sequence1[-r+1] : sequence1[r-1];
      c2 = revp ? sequence2[-c+1] : sequence2[c-1];
      if (toupper(c1) == toupper(c2)) {
	*nmatches += 1;
      } else {
	*nmismatches += 1;
      }
      r--; c--;
    } else if (directions[r][c] == HORIZ) {
      debug(printf("H%d: ",jump[r][c]));
      *nopens += 1;
      *nindels += (dist = jump[r][c]);
      c -= dist;
    } else if (directions[r][c] == VERT) {
      debug(printf("V%d: ",jump[r][c]));
      *nopens += 1;
      *nindels += (dist = jump[r][c]);
      r -= dist;
    } else {
      abort();
    }
  }

  return;
}


/* We have switched length2 for columns, and length 1L and 1R for rows */
static void
bridge_cdna_gap (int *finalscore, int *bestrL, int *bestrR, int *bestcL, int *bestcR,
		 int **matrixL, int **matrixR, Direction_T **directionsL, Direction_T **directionsR, 
		 int length2, int length1L, int length1R, int extraband_paired,
		 int open, int extend, int leftoffset, int rightoffset) {
  Direction_T lastdir = DIAG;
  int bestscore = -100000, scoreL, scoreR;
  int rL, rR, cL, cR;
  int lbandL, rbandL, cloL, chighL;
  int lbandR, rbandR, cloR, chighR;
  char left1, left2, right2, right1;
  int pen, end_reward;

  /* Perform computations */
  rbandL = length1L - length2 + extraband_paired;
  lbandL = extraband_paired;

  rbandR = length1R - length2 + extraband_paired;
  lbandR = extraband_paired;

  /* Need a double loop on rows here, in contrast with a single loop
     for introns, because we allow a genomic insertion that doesn't
     match the cDNA.  So we need to add a penalty for a genomic
     insertion */
  for (rL = 0; rL <= length2; rL++) {
    if (rL == 0 || rL == length2) {
      end_reward = 1;
    } else {
      end_reward = 0;
    }

    /* Note: opening penalty is added at the bottom of the loop */
    for (rR = length2-rL, pen = 0; rR >= 0; rR--, pen += extend) {
      debug3(printf("\nAt row %d on left and %d on right\n",rL,rR));
      if ((cloL = rL - lbandL) < 0) {
	cloL = 0;
      }
      if ((chighL = rL + rbandL) > length1L) {
	chighL = length1L;
      }

      if ((cloR = rR - lbandR) < 0) {
	cloR = 0;
      }
      if ((chighR = rR + rbandR) > length1R) {
	chighR = length1R;
      }

      for (cL = cloL; cL <= chighL; cL++) {
	scoreL = matrixL[rL][cL];
	
	for (cR = cloR; cR <= chighR; cR++) {
	  scoreR = matrixR[rR][cR];

	  if (scoreL + scoreR + pen + end_reward > bestscore) {
	    debug3(printf("At %d left to %d right, score is (%d)+(%d)+(%d)+(%d) = %d (bestscore)\n",
			  cL,cR,scoreL,scoreR,pen,end_reward,scoreL+scoreR+pen+end_reward));
#if 0
	    if (leftoffset + cL >= rightoffset - cR) {
	      debug3(printf("  Disallowed because %d >= %d\n",leftoffset+cL,rightoffset-cR));
	    } else {
#endif
	      bestscore = scoreL + scoreR + pen + end_reward;
	      *bestrL = rL;
	      *bestrR = rR;
	      *bestcL = cL;
	      *bestcR = cR;
#if 0
	    }
#endif
	  } else {
	    /*
	    debug3(printf("At %d left to %d right, score is (%d)+(%d)+(%d) = %d\n",
			  cL,cR,scoreL,scoreR,pen,scoreL+scoreR+pen));
	    */
	  }
	}
      }
      pen = open - extend;	/* Subtract extend to compensate for
                                   its addition in the for loop */
    }
  }
      
  *finalscore = bestscore;
  debug3(printf("Returning final score of %d at (%d,%d) left to (%d,%d) right\n",
		*finalscore,*bestrL,*bestcL,*bestrR,*bestcR));

  return;
}

static int
intron_score (int *introntype, int leftdi, int rightdi, int cdna_direction, int canonical_reward, bool endp) {
  int scoreI;

  if ((*introntype = leftdi & rightdi) == NONINTRON) {
    scoreI = 0.0;
  } else if (cdna_direction > 0) {
    switch (*introntype) {
    case GTAG_FWD: scoreI = canonical_reward; break;
    case GCAG_FWD: case ATAC_FWD: 
      if (endp == true) {
	*introntype = NONINTRON; scoreI = 0.0;
      } else {
	scoreI = ALTERNATE_INTRON;
      }
      break;
    default: *introntype = NONINTRON; scoreI = 0.0;
    }
  } else if (cdna_direction < 0) {
    switch (*introntype) {
    case GTAG_REV: scoreI = canonical_reward; break;
    case GCAG_REV: case ATAC_REV:
      if (endp == true) {
	*introntype = NONINTRON; 
	scoreI = 0.0;
      } else {
	scoreI = ALTERNATE_INTRON;
      }
      break;
    default: *introntype = NONINTRON; scoreI = 0.0;
    }
  } else {
    switch (*introntype) {
    case GTAG_FWD: case GTAG_REV: scoreI = canonical_reward; break;
    case GCAG_FWD: case GCAG_REV: case ATAC_FWD: case ATAC_REV: 
      if (endp == true) {
	*introntype = NONINTRON;	      
	scoreI = 0.0;
      } else {
	scoreI = ALTERNATE_INTRON;
      }
      break;
    default: *introntype = NONINTRON; scoreI = 0.0;
    }
  }
  return scoreI;
}


static void
bridge_intron_gap (int *finalscore, int *bestrL, int *bestrR, int *bestcL, int *bestcR, int *best_introntype, 
		   int **matrixL, int **matrixR, Direction_T **directionsL, Direction_T **directionsR, 
		   int **jumpL, int **jumpR,  char *sequence2L, char *revsequence2R, 
		   int length1, int length2L, int length2R,
		   int cdna_direction, int extraband_paired, bool endp, int canonical_reward,
		   int maxhorizjump, int maxvertjump, int leftoffset, int rightoffset) {
  Direction_T lastdir = DIAG;
  int bestscore = -100000, scoreL, scoreR, scoreI;
  int rL, rR, cL, cR;
  int lbandL, rbandL, cloL, chighL;
  int lbandR, rbandR, cloR, chighR;
  char left1, left2, right2, right1;
  int *leftdi, *rightdi, introntype;

  /* Read dinucleotides */
  leftdi = (int *) CALLOC(length2L+1,sizeof(int));
  rightdi = (int *) CALLOC(length2R+1,sizeof(int));

  for (cL = 0; cL < length2L - 1; cL++) {
    left1 = sequence2L[cL];
    left2 = sequence2L[cL+1];
    if (left1 == 'G' && left2 == 'T') {
      leftdi[cL] = LEFT_GT;
    } else if (left1 == 'G' && left2 == 'C') {
      leftdi[cL] = LEFT_GC;
    } else if (left1 == 'A' && left2 == 'T') {
      leftdi[cL] = LEFT_AT;
    } else if (left1 == 'C' && left2 == 'T') {
      leftdi[cL] = LEFT_CT;
    } else {
      leftdi[cL] = 0x00;
    }
  }
  leftdi[length2L-1] = leftdi[length2L] = 0x00;

  for (cR = 0; cR < length2R - 1; cR++) {
    right2 = revsequence2R[-cR-1];
    right1 = revsequence2R[-cR];
    if (right2 == 'A' && right1 == 'G') {
      rightdi[cR] = RIGHT_AG;
    } else if (right2 == 'A' && right1 == 'C') {
      rightdi[cR] = RIGHT_AC;
    } else if (right2 == 'G' && right1 == 'C') {
      rightdi[cR] = RIGHT_GC;
    } else if (right2 == 'A' && right1 == 'T') {
      rightdi[cR] = RIGHT_AT;
    } else {
      rightdi[cR] = 0x00;
    }
  }
  rightdi[length2R-1] = rightdi[length2R] = 0x00;

  /* Perform computations */
  rbandL = length2L - length1 + extraband_paired;
  lbandL = extraband_paired;

  rbandR = length2R - length1 + extraband_paired;
  lbandR = extraband_paired;

  for (rL = 0, rR = length1; rL <= length1; rL++, rR--) {
    debug3(printf("\nAt row %d on left and %d on right\n",rL,rR));
    if ((cloL = rL - lbandL) < 0) {
      cloL = 0;
    }
    if ((chighL = rL + rbandL) > length2L) {
      chighL = length2L;
    }

    if ((cloR = rR - lbandR) < 0) {
      cloR = 0;
    }
    if ((chighR = rR + rbandR) > length2R) {
      chighR = length2R;
    }

    for (cL = cloL; cL <= chighL; cL++) {
      /* The following check limits genomic inserts (horizontal) and
         multiple cDNA inserts (vertical). */
      if (directionsL[rL][cL] == DIAG ||
	  (directionsL[rL][cL] == HORIZ && jumpL[rL][cL] <= maxhorizjump) ||
	  (directionsL[rL][cL] == VERT && jumpL[rL][cL] <= maxvertjump) ||
	  directionsL[rL][cL] == STOP) {
	scoreL = matrixL[rL][cL];
	
	if (directionsL[rL][cL] == HORIZ || directionsL[rL][cL] == VERT) {
	  /* Favor gaps away from intron if possible */
	  scoreL -= 1;
	}

	for (cR = cloR; cR <= chighR; cR++) {
	  if (directionsR[rR][cR] == DIAG ||
	      (directionsR[rR][cR] == HORIZ && jumpR[rR][cR] <= maxhorizjump) ||
	      (directionsR[rR][cR] == VERT && jumpR[rR][cR] <= maxvertjump) ||
	      directionsR[rR][cR] == STOP) {
	    scoreR = matrixR[rR][cR];

	    if (directionsR[rR][cR] == HORIZ || directionsR[rR][cR] == VERT) {
	      /* Favor gaps away from intron if possible */
	      scoreR -= 1;
	    }
	    
	    scoreI = intron_score(&introntype,leftdi[cL],rightdi[cR],
				  cdna_direction,canonical_reward,endp);
	    
	    if (scoreL + scoreI + scoreR > bestscore) {
	      debug3(printf("At %d left to %d right, score is (%d)+(%d)+(%d) = %d (bestscore)\n",
			    cL,cR,scoreL,scoreI,scoreR,scoreL+scoreI+scoreR));
#if 0
	      if (leftoffset + cL >= rightoffset - cR) {
		debug3(printf("  Disallowed because %d >= %d\n",leftoffset+cL,rightoffset-cR));
	      } else {
#endif
		debug3(printf("  Allowed because %d < %d\n",leftoffset+cL,rightoffset-cR));
		bestscore = scoreL + scoreI + scoreR;
		*bestrL = rL;
		*bestrR = rR;
		*bestcL = cL;
		*bestcR = cR;
		*best_introntype = introntype;
#if 0
	      }
#endif
	    } else {
	      /*
		debug3(printf("At %d left to %d right, score is (%d)+(%d)+(%d) = %d\n",
		cL,cR,scoreL,scoreI,scoreR,scoreL+scoreI+scoreR));
	      */
	    }
	  }
	}
      }
    }
  }
      
  *finalscore = bestscore;
  debug3(printf("Returning final score of %d at (%d,%d) left to (%d,%d) right\n",
		*finalscore,*bestrL,*bestcL,*bestrR,*bestcR));
  FREE(rightdi);
  FREE(leftdi);

  return;
}


/************************************************************************/

List_T
Dynprog_single_gap (int *finalscore, int *nmatches, int *nmismatches, int *nopens, int *nindels,
		    T dynprog, char *sequence1, char *sequence2, 
		    int length1, int length2, int offset1, int offset2,
		    Pairpool_T pairpool, int extraband_single, double defect_rate) {
  List_T pairs = NULL;
  int **matrix, **jump, mismatch, open, extend;
  Direction_T **directions;

  /* Length1: maxlookback+MAXPEELBACK.  Length2 +EXTRAMATERIAL */
  /*
  if (length1 > dynprog->maxlength1 || length2 > dynprog->maxlength2) {
    fprintf(stderr,"%d %d\n",length1,length2);
    abort();
  }
  */

  debug(
	printf("Aligning single gap middle\n");
	printf("At query offset %d-%d, %.*s\n",offset1,offset1+length1-1,length1,sequence1);
	printf("At genomic offset %d-%d, %.*s\n",offset2,offset2+length2-1,length2,sequence2);
	printf("\n");
	);

  if (defect_rate < DEFECT_HIGHQ) {
    mismatch = MISMATCH_HIGHQ;
    open = SINGLE_OPEN_HIGHQ;
    extend = SINGLE_EXTEND_HIGHQ;
  } else if (defect_rate < DEFECT_MEDQ) {
    mismatch = MISMATCH_MEDQ;
    open = SINGLE_OPEN_MEDQ;
    extend = SINGLE_EXTEND_MEDQ;
  } else {
    mismatch = MISMATCH_LOWQ;
    open = SINGLE_OPEN_LOWQ;
    extend = SINGLE_EXTEND_LOWQ;
  }

  /* endp is false */
  matrix = compute_scores(&directions,&jump,dynprog,sequence1,sequence2,
			  /*revp*/false,length1,length2,
			  mismatch,open,extend,extraband_single);

  *finalscore = matrix[length1][length2];
  *nmatches = *nmismatches = *nopens = *nindels = 0;
  pairs = traceback(NULL,&(*nmatches),&(*nmismatches),&(*nopens),&(*nindels),
		    directions,jump,length1,length2,
		    sequence1,sequence2,offset1,offset2,pairpool,
		    /*revp*/false,/*ngap*/0,/*cdna_direction*/0);

  /*
  Directions_free(directions);
  Matrix_free(matrix);
  */

  return List_reverse(pairs);
}




/* Sequence 1L and 1R represent the two ends of the cDNA insertion */
List_T
Dynprog_cdna_gap (int *finalscore, T dynprogL, T dynprogR, 
		  char *sequence1L, char *revsequence1R, char *sequence2,
		  int length1L, int length1R, int length2,
		  int offset1L, int revoffset1R, int offset2,
		  Pairpool_T pairpool, int extraband_paired, double defect_rate) {
  List_T pairs = NULL, p;
  Pair_T pair;
  char *revsequence2;
  int **matrixL, **matrixR, **jumpL, **jumpR, mismatch, open, extend;
  Direction_T **directionsL, **directionsR;
  int revoffset2, bestrL, bestrR, bestcL, bestcR, k;
  int nmatches, nmismatches, nopens, nindels;

  if (length2 <= 0) {
    return NULL;
  }

  /*
  if (length1 > dynprogL->maxlength1 || length2L > dynprogL->maxlength2 || length2R > dynprogR->maxlength2) {
    fprintf(stderr,"%d %d %d\n",length1,length2L,length2R);
    abort();
  }
  */

  debug(
	printf("\n");
	printf("Aligning cdna gap\n");
	printf("At query offset %d-%d, %.*s\n",offset1L,offset1L+length1L-1,length1L,sequence1L);
	printf("At query offset %d-%d, %.*s\n",revoffset1R-length1R+1,revoffset1R,length1R,&(revsequence1R[-length1R+1]));
	printf("At genomic offset %d-%d, %.*s\n",offset2,offset2+length2-1,length2,sequence2);
	printf("\n");
	);

  /* ?check if offsets are too close.  But this eliminates a segment
     of the cDNA.  Should check in stage 3, and do single gap instead. */
  /*
  if (offset1L+length1L-1 >= revoffset1R-length1R+1) {
    debug(printf("Bounds don't make sense\n"));
    *finalscore = -100000.0;
    return NULL;
  }
  */

  if (defect_rate < DEFECT_HIGHQ) {
    mismatch = MISMATCH_HIGHQ;
    open = CDNA_OPEN_HIGHQ;
    extend = CDNA_EXTEND_HIGHQ;
  } else if (defect_rate < DEFECT_MEDQ) {
    mismatch = MISMATCH_MEDQ;
    open = CDNA_OPEN_MEDQ;
    extend = CDNA_EXTEND_MEDQ;
  } else {
    mismatch = MISMATCH_LOWQ;
    open = CDNA_OPEN_LOWQ;
    extend = CDNA_EXTEND_LOWQ;
  }

  /* Right side looks like 5' end */
  /* Note: sequence 1 and 2 flipped, because 1 has extramaterial */
  revsequence2 = &(sequence2[length2-1]);
  revoffset2 = offset2+length2-1;
  matrixR = compute_scores(&directionsR,&jumpR,dynprogR,
			   revsequence2,revsequence1R,/*revp*/true,length2,length1R,
			   mismatch,open,extend,extraband_paired);

  /* Left side looks like 3' end */
  /* Note: sequence 1 and 2 flipped, because 1 has extramaterial */
  /* endp is false */
  matrixL = compute_scores(&directionsL,&jumpL,dynprogL,
			   sequence2,sequence1L,/*revp*/false,length2,length1L,
			   mismatch,open,extend,extraband_paired);

  bridge_cdna_gap(&(*finalscore),&bestrL,&bestrR,&bestcL,&bestcR,matrixL,matrixR,
		  directionsL,directionsR,length2,length1L,length1R,extraband_paired,
		  open,extend,offset1L,revoffset1R);

  nmatches = nmismatches = nopens = nindels = 0;
  pairs = traceback(NULL,&nmatches,&nmismatches,&nopens,&nindels,
		    directionsR,jumpR,bestrR,bestcR,
		    revsequence2,revsequence1R,revoffset2,revoffset1R,
		    pairpool,/*revp*/true,/*ngap*/0,/*cdna_direction*/0);
  pairs = List_reverse(pairs);

  /* cDNA and genome are flipped here.  Use c instead of r.  Unflip later. */
  /* Add genome insertion, if any */
  debug(printf("\n"));
  for (k = revoffset2-bestrR; k >= offset2+bestrL; k--) {
    debug(printf("genome insertion, Pushing %d (%c)\n",k+1,sequence2[k-offset2]));
    pairs = Pairpool_push(pairs,pairpool,k,revoffset1R-bestcR+1,sequence2[k-offset2],'-',' ');
  }
  debug(printf("\n"));

  /* Add cDNA insertion */
  for (k = revoffset1R-bestcR; k >= offset1L+bestcL; k--) {
    debug(printf("cDNA insertion, Pushing %d (%c)\n",k+1,sequence1L[k-offset1L]));
    pairs = Pairpool_push(pairs,pairpool,offset2+bestrL,k,' ','-',sequence1L[k-offset1L]);
  }
  debug(printf("\n"));

  pairs = traceback(pairs,&nmatches,&nmismatches,&nopens,&nindels,
		    directionsL,jumpL,bestrL,bestcL,
		    sequence2,sequence1L,offset2,offset1L,pairpool,
		    /*revp*/false,/*ngap*/0,/*cdna_direction*/0);

  /* Unflip cDNA and genome */
  for (p = pairs; p != NULL; p = List_next(p)) {
    pair = (Pair_T) List_head(p);
    Pair_flip(pair);
  }

  /*
  Directions_free(directionsR);
  Directions_free(directionsL);
  Matrix_free(matrixR);
  Matrix_free(matrixL);
  */

  return List_reverse(pairs);
}


/* A genome gap is usually an intron.  Sequence 2L and 2R represent
   the two genomic ends of the intron. */
List_T
Dynprog_genome_gap (int *finalscore, int *nmatches, int *nmismatches, int *nopens, int *nindels,
		    int *exonhead, int *introntype, T dynprogL, T dynprogR, 
		    char *sequence1, char *sequence2L, char *revsequence2R, 
		    int length1, int length2L, int length2R, 
		    int offset1, int offset2L, int revoffset2R, int cdna_direction,
		    int ngap, Pairpool_T pairpool, int extraband_paired,
		    bool endp, double defect_rate, bool returnpairsp, bool addgapp) {
  List_T pairs = NULL;
  char *revsequence1;
  int **matrixL, **matrixR, **jumpL, **jumpR, mismatch, open, extend;
  int canonical_reward;
  Direction_T **directionsL, **directionsR;
  int revoffset1, bestrL, bestrR, bestcL, bestcR;
  int maxhorizjump, maxvertjump;

  /*
  if (length1 > dynprogL->maxlength1 || length2L > dynprogL->maxlength2 || length2R > dynprogR->maxlength2) {
    fprintf(stderr,"%d %d %d\n",length1,length2L,length2R);
    abort();
  }
  */

  debug(
	printf("\n");
	printf("Aligning genome gap with cdna_direction %d\n",cdna_direction);
	printf("At query offset %d-%d, %.*s\n",offset1,offset1+length1-1,length1,sequence1);
	printf("At genomic offset %d-%d, %.*s\n",offset2L,offset2L+length2L-1,length2L,sequence2L);
	printf("At genomic offset %d-%d, %.*s\n",revoffset2R-length2R+1,revoffset2R,length2R,&(revsequence2R[-length2R+1]));
	printf("\n");
	);

  /* ?check if offsets are too close.  But this eliminates a segment
     of the cDNA.  Should check in stage 3, and do single gap instead. */
  /*
  if (offset2L+length2L-1 >= revoffset2R-length2R+1) {
    debug(printf("Bounds don't make sense\n"));
    *finalscore = -100000.0;
    return NULL;
  }
  */

  if (defect_rate < DEFECT_HIGHQ) {
    mismatch = MISMATCH_HIGHQ;
    open = PAIRED_OPEN_HIGHQ;
    extend = PAIRED_EXTEND_HIGHQ;
    canonical_reward = CANONICAL_INTRON_HIGHQ;
    maxhorizjump = MAXHORIZJUMP_HIGHQ;
    maxvertjump = MAXVERTJUMP_HIGHQ;
  } else if (defect_rate < DEFECT_MEDQ) {
    mismatch = MISMATCH_MEDQ;
    open = PAIRED_OPEN_MEDQ;
    extend = PAIRED_EXTEND_MEDQ;
    canonical_reward = CANONICAL_INTRON_MEDQ;
    maxhorizjump = MAXHORIZJUMP_MEDQ;
    maxvertjump = MAXVERTJUMP_MEDQ;
  } else {
    mismatch = MISMATCH_LOWQ;
    open = PAIRED_OPEN_LOWQ;
    extend = PAIRED_EXTEND_LOWQ;
    canonical_reward = CANONICAL_INTRON_LOWQ;
    maxhorizjump = MAXHORIZJUMP_LOWQ;
    maxvertjump = MAXVERTJUMP_LOWQ;
  }

  matrixL = compute_scores(&directionsL,&jumpL,dynprogL,
			   sequence1,sequence2L,/*revp*/false,length1,length2L,
			   mismatch,open,extend,extraband_paired);

  revsequence1 = &(sequence1[length1-1]);
  revoffset1 = offset1+length1-1;
  matrixR = compute_scores(&directionsR,&jumpR,dynprogR,
			   revsequence1,revsequence2R,/*revp*/true,length1,length2R,
			   mismatch,open,extend,extraband_paired);

  bridge_intron_gap(&(*finalscore),&bestrL,&bestrR,&bestcL,&bestcR,&(*introntype),matrixL,matrixR,
		    directionsL,directionsR,jumpL,jumpR,sequence2L,revsequence2R,
		    length1,length2L,length2R,cdna_direction,extraband_paired,endp,
		    canonical_reward,maxhorizjump,maxvertjump,offset2L,revoffset2R);
  debug(printf("Intron is between %d and %d\n",offset1+(bestrL-1),revoffset1-(bestrR-1)));
  *exonhead = revoffset1-(bestrR-1);

  *nmatches = *nmismatches = *nopens = *nindels = 0;
  if (returnpairsp == false) {
    countback(&(*nmatches),&(*nmismatches),&(*nopens),&(*nindels),
	      directionsR,jumpR,bestrR,bestcR,
	      revsequence1,revsequence2R,revoffset1,revoffset2R,
	      pairpool,/*revp*/true);
    countback(&(*nmatches),&(*nmismatches),&(*nopens),&(*nindels),
	      directionsL,jumpL,bestrL,bestcL,
	      sequence1,sequence2L,offset1,offset2L,pairpool,/*revp*/false);

  } else {
    pairs = traceback(NULL,&(*nmatches),&(*nmismatches),&(*nopens),&(*nindels),
		      directionsR,jumpR,bestrR,bestcR,
		      revsequence1,revsequence2R,revoffset1,revoffset2R,
		      pairpool,/*revp*/true,ngap,cdna_direction);
    pairs = List_reverse(pairs);
    if (addgapp) {
      pairs = add_gap(pairs,bestrL,bestcL,bestcR,sequence1,sequence2L,revsequence2R,
		      length1,length2L,length2R,offset1,offset2L,revoffset2R,
		      *introntype,ngap,pairpool);
    }
    pairs = traceback(pairs,&(*nmatches),&(*nmismatches),&(*nopens),&(*nindels),
		      directionsL,jumpL,bestrL,bestcL,
		      sequence1,sequence2L,offset1,offset2L,pairpool,/*revp*/false,
		      ngap,cdna_direction);
  }

  /*
  Directions_free(directionsR);
  Directions_free(directionsL);
  Matrix_free(matrixR);
  Matrix_free(matrixL);
  */

  return List_reverse(pairs);
}


List_T
Dynprog_end5_gap (int *finalscore, int *nmatches, int *nmismatches, int *nopens, int *nindels,
		  T dynprog, char *revsequence1, char *revsequence2,
		  int length1, int length2, int revoffset1, int revoffset2, 
		  Pairpool_T pairpool, int extraband_end, double defect_rate,
		  int cdna_direction, int ngap, bool extend_mismatch_p) {
  List_T pairs = NULL;
  Pair_T pair;
  int **matrix, **jump, mismatch, open, extend;
  int bestr, bestc;
  Direction_T **directions;

  debug(
	printf("\n");
	printf("Aligning 5' end gap\n");
	printf("At query offset %d-%d, %.*s\n",revoffset1-length1+1,revoffset1,length1,&(revsequence1[-length1+1]));
	printf("At genomic offset %d-%d, %.*s\n",revoffset2-length2+1,revoffset2,length2,&(revsequence2[-length2+1]));
	printf("\n")
	);

  if (defect_rate < DEFECT_HIGHQ) {
    mismatch = END_MISMATCH_HIGHQ;
    open = END_OPEN_HIGHQ;
    extend = END_EXTEND_HIGHQ;
  } else if (defect_rate < DEFECT_MEDQ) {
    mismatch = END_MISMATCH_MEDQ;
    open = END_OPEN_MEDQ;
    extend = END_EXTEND_MEDQ;
  } else {
    mismatch = END_MISMATCH_LOWQ;
    open = END_OPEN_LOWQ;
    extend = END_EXTEND_LOWQ;
  }

  matrix = compute_scores(&directions,&jump,dynprog,
			  revsequence1,revsequence2,/*revp*/true,length1,length2,
			  mismatch,open,extend,extraband_end);

  find_best_endpoint_onegap(&(*finalscore),&bestr,&bestc,matrix,length1,length2,
			    extraband_end,extend_mismatch_p);
  *nmatches = *nmismatches = *nopens = *nindels = 0;
  pairs = traceback(NULL,&(*nmatches),&(*nmismatches),&(*nopens),&(*nindels),
		    directions,jump,bestr,bestc,
		    revsequence1,revsequence2,revoffset1,revoffset2,
		    pairpool,/*revp*/true,ngap,cdna_direction);
  if ((*nmatches + 1) >= *nmismatches) {
    /* Add 1 to count the match already in the alignment */
    pairs = List_reverse(pairs); /* Look at 5' end to remove excess gaps */
    while (pairs != NULL && (pair = List_head(pairs)) && pair->comp == '-') {
      pairs = List_next(pairs);
    }
    return List_reverse(pairs);
  } else {
    *finalscore = 0;
    /* No need to free pairs */
    return NULL;
  }

  /*
    Directions_free(directions);
    Matrix_free(matrix);
  */
}


List_T
Dynprog_end3_gap (int *finalscore, int *nmatches, int *nmismatches, int *nopens, int *nindels,
		  T dynprog, char *sequence1, char *sequence2,
		  int length1, int length2, int offset1, int offset2, 
		  Pairpool_T pairpool, int extraband_end, double defect_rate,
		  int cdna_direction, int ngap, bool extend_mismatch_p) {
  List_T pairs = NULL;
  Pair_T pair;
  int **matrix, **jump, mismatch, open, extend;
  int bestr, bestc;
  Direction_T **directions;

  debug(
	printf("\n");
	printf("Aligning 3' end gap\n");
	printf("At query offset %d-%d, %.*s\n",offset1,offset1+length1-1,length1,sequence1);
	printf("At genomic offset %d-%d, %.*s\n",offset2,offset2+length2-1,length2,sequence2);
	printf("\n")
	);

  if (defect_rate < DEFECT_HIGHQ) {
    mismatch = END_MISMATCH_HIGHQ;
    open = END_OPEN_HIGHQ;
    extend = END_EXTEND_HIGHQ;
  } else if (defect_rate < DEFECT_MEDQ) {
    mismatch = END_MISMATCH_MEDQ;
    open = END_OPEN_MEDQ;
    extend = END_EXTEND_MEDQ;
  } else {
    mismatch = END_MISMATCH_LOWQ;
    open = END_OPEN_LOWQ;
    extend = END_EXTEND_LOWQ;
  }

  /* endp is true */
  matrix = compute_scores(&directions,&jump,dynprog,
			  sequence1,sequence2,/*revp*/false,length1,length2,
			  mismatch,open,extend,extraband_end);

  find_best_endpoint_onegap(&(*finalscore),&bestr,&bestc,matrix,length1,length2,
			    extraband_end,extend_mismatch_p);
  *nmatches = *nmismatches = *nopens = *nindels = 0;
  pairs = traceback(NULL,&(*nmatches),&(*nmismatches),&(*nopens),&(*nindels),
		    directions,jump,bestr,bestc,
		    sequence1,sequence2,offset1,offset2,pairpool,
		    /*revp*/false,ngap,cdna_direction);

  if ((*nmatches + 1) >= *nmismatches) {
    /* Add 1 to count the match already in the alignment */
    pairs = List_reverse(pairs); /* Look at 3' end to remove excess gaps */
    while (pairs != NULL && (pair = List_head(pairs)) && pair->comp == '-') {
      pairs = List_next(pairs);
    }
    return pairs;
  } else {
    *finalscore = 0;
    /* No need to free pairs */
    return NULL;
  }

  /*
    Directions_free(directions);
    Matrix_free(matrix);
  */
}



static List_T
make_microexon_pairs_double (int offset1L, int offset1M, int offset1R,
			     int offset2L, int offset2M, int offset2R,
			     int lengthL, int lengthM, int lengthR,
			     char *queryseq, char *genomicseg, 
			     Pairpool_T pairpool, int ngap, char gapchar) {
  List_T pairs = NULL;
  char match = '*', c1, c2;
  int i, j, k;

  /* Left segment */
  for (i = 0; i < lengthL; i++) {
    c1 = queryseq[offset1L+i];
    c2 = genomicseg[offset2L+i];
    if (toupper(c1) == toupper(c2)) {
      pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+i,c1,match,c2);
    } else {
      pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+i,c1,' ',c2);
    }
  }

  /* First gap */
  j = i-1;
  for (k = 0; k < ngap; k++) {
    c2 = genomicseg[offset2L+(++j)];
    pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+j,' ',gapchar,c2);
  }

  pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+j,' ','.','.');
  pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+j,' ','.','.');
  pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+j,' ','.','.');

  for (j = ngap; j >= 1; j--) {
    c2 = genomicseg[offset2M-j];
    pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2M-j,' ',gapchar,c2);
  }
  
  /* Microexon */
  for (i = 0; i < lengthM; i++) {
    c1 = queryseq[offset1M+i];
    c2 = genomicseg[offset2M+i];
    if (toupper(c1) == toupper(c2)) {
      pairs = Pairpool_push(pairs,pairpool,offset1M+i,offset2M+i,c1,match,c2);
    } else {
      pairs = Pairpool_push(pairs,pairpool,offset1M+i,offset2M+i,c1,' ',c2);
    }
  }

  /* Second gap */
  j = i-1;
  for (k = 0; k < ngap; k++) {
    c2 = genomicseg[offset2M+(++j)];
    pairs = Pairpool_push(pairs,pairpool,offset1M+i,offset2M+j,' ',gapchar,c2);
  }

  pairs = Pairpool_push(pairs,pairpool,offset1M+i,offset2M+j,' ','.','.');
  pairs = Pairpool_push(pairs,pairpool,offset1M+i,offset2M+j,' ','.','.');
  pairs = Pairpool_push(pairs,pairpool,offset1M+i,offset2M+j,' ','.','.');

  for (j = ngap; j >= 1; j--) {
    c2 = genomicseg[offset2R-j];
    pairs = Pairpool_push(pairs,pairpool,offset1M+i,offset2R-j,' ',gapchar,c2);
  }
  
  /* Right segment */
  for (i = 0; i < lengthR; i++) {
    c1 = queryseq[offset1R+i];
    c2 = genomicseg[offset2R+i];
    if (toupper(c1) == toupper(c2)) {
      pairs = Pairpool_push(pairs,pairpool,offset1R+i,offset2R+i,c1,match,c2);
    } else {
      pairs = Pairpool_push(pairs,pairpool,offset1R+i,offset2R+i,c1,' ',c2);
    }
  }

  return pairs;
}


static List_T
make_microexon_pairs_single (int offset1L, int offset1R,
			     int offset2L, int offset2R,
			     int lengthL, int lengthR,
			     char *queryseq, char *genomicseg, 
			     Pairpool_T pairpool, int ngap, char gapchar) {
  List_T pairs = NULL;
  char match = '*', c1, c2;
  int i, j, k;

  /* Microexon */
  for (i = 0; i < lengthL; i++) {
    c1 = queryseq[offset1L+i];
    c2 = genomicseg[offset2L+i];
    if (toupper(c1) == toupper(c2)) {
      pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+i,c1,match,c2);
    } else {
      pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+i,c1,' ',c2);
    }
  }

  /* Gap */
  j = i-1;
  for (k = 0; k < ngap; k++) {
    c2 = genomicseg[offset2L+(++j)];
    pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+j,' ',gapchar,c2);
  }

  pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+j,' ','.','.');
  pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+j,' ','.','.');
  pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2L+j,' ','.','.');

  for (j = ngap; j >= 1; j--) {
    c2 = genomicseg[offset2R-j];
    pairs = Pairpool_push(pairs,pairpool,offset1L+i,offset2R-j,' ',gapchar,c2);
  }
  
  /* Right segment */
  for (i = 0; i < lengthR; i++) {
    c1 = queryseq[offset1R+i];
    c2 = genomicseg[offset2R+i];
    if (toupper(c1) == toupper(c2)) {
      pairs = Pairpool_push(pairs,pairpool,offset1R+i,offset2R+i,c1,match,c2);
    } else {
      pairs = Pairpool_push(pairs,pairpool,offset1R+i,offset2R+i,c1,' ',c2);
    }
  }

  return pairs;
}

List_T
Dynprog_microexon_int (int *microintrontype, char *sequence1, char *sequence2L, char *revsequence2R,
		       int length1, int length2L, int length2R,
		       int offset1, int offset2L, int revoffset2R,
		       int cdna_direction, char *queryseq, char *genomicseg, 
		       Pairpool_T pairpool, int ngap) {
  Intlist_T hits = NULL, p;
  int middlelength, cL, cR, mincR, maxcR, leftbound, rightbound, textleft, textright, candidate, i;
  int min_microexon_length, span, nmismatches;
  char left1, left2, right2, right1;
  char intron1, intron2, intron3, intron4, gapchar;

  if (cdna_direction > 0) {
    intron1 = 'G';
    intron2 = 'T';
    intron3 = 'A';
    intron4 = 'G';
    gapchar = '>';
    *microintrontype = GTAG_FWD;
  } else if (cdna_direction < 0) {
    intron1 = 'C';
    intron2 = 'T';
    intron3 = 'A';
    intron4 = 'C';
    gapchar = '<';
    *microintrontype = GTAG_REV;
  } else {
    abort();
  }

  debug(printf("Begin microexon search for %.*s and %.*s\n",
	       length2L,sequence2L,length2R,&(revsequence2R[-length2R+1])));
  debug(printf("  Query sequence is %.*s\n",length1,sequence1));
  span = revoffset2R-offset2L;
  debug(printf("  Genomic span is of length %d\n",span));

  if (span <= 0) {
    fprintf(stderr,"Bug in Dynprog_microexon_int.  span = %d.  Please report to twu@gene.com\n",span);
    abort();
  } else {
    min_microexon_length = ceil(-log(1.0-pow(1.0-MICROEXON_PVALUE,1.0/(double) span))/log(4));
  }
  min_microexon_length -= 8;	/* Two donor-acceptor pairs */
  debug(printf("  Min microexon length is %d\n",min_microexon_length));
  if (min_microexon_length > MAX_MICROEXON_LENGTH) {
    return NULL;
  } else if (min_microexon_length < MIN_MICROEXON_LENGTH) {
    min_microexon_length = MIN_MICROEXON_LENGTH;
  }

  leftbound = 0;
  nmismatches = 0;
  while (leftbound < length1 - 1 && nmismatches <= 1) {
    if (sequence1[leftbound] != sequence2L[leftbound]) {
      nmismatches++;
    }
    leftbound++;
  }
  leftbound--;			/* This is where the last mismatch occurred */

  rightbound = 0;
  i = length1-1;
  nmismatches = 0;
  while (i >= 0 && nmismatches <= 1) {
    if (sequence1[i] != revsequence2R[-rightbound]) {
      nmismatches++;
    }
    rightbound++;
    i--;
  }
  rightbound--;			/* This is where the last mismatch occurred */

  debug(printf("  Left must start before %d from left end.  Right must start after %d from right end\n",
	       leftbound,rightbound));

  for (cL = 0; cL <= leftbound; cL++) {
    left1 = sequence2L[cL];
    left2 = sequence2L[cL+1];
    debug(printf("  %d: %c%c\n",cL,left1,left2));
    if (left1 == intron1 && left2 == intron2) {
      mincR = length1 - MAX_MICROEXON_LENGTH - cL;
      if (mincR < 0) {
	mincR = 0;
      }
      maxcR = length1 - min_microexon_length - cL;
      if (maxcR > rightbound) {
	maxcR = rightbound;
	}
      debug(printf("  Found left GT at %d.  Scanning from %d - cL - (1-7), or %d to %d\n",
		   cL,length1,mincR,maxcR));
      for (cR = mincR; cR <= maxcR; cR++) {
	right2 = revsequence2R[-cR-1];
	right1 = revsequence2R[-cR];
	debug(printf("   Checking %d: %c%c\n",cR,right2,right1));
	if (right2 == intron3 && right1 == intron4) {
	  middlelength = length1 - cL - cR;
	  debug(printf("  Found pair at %d to %d, length %d.  Middle sequence is %.*s\n",
		       cL,cR,middlelength,middlelength,&(sequence1[cL])));
	  
	  textleft = offset2L + cL + MICROINTRON_LENGTH;
	  textright = revoffset2R - cR - MICROINTRON_LENGTH;
	  hits = BoyerMoore(&(sequence1[cL]),middlelength,&(genomicseg[textleft]),textright-textleft);
	  for (p = hits; p != NULL; p = Intlist_next(p)) {
	    candidate = textleft + Intlist_head(p);
	    if (genomicseg[candidate - 2] == intron3 &&
		genomicseg[candidate - 1] == intron4 &&
		genomicseg[candidate + middlelength] == intron1 &&
		genomicseg[candidate + middlelength + 1] == intron2) {
	      debug(printf("  Successful microexon at %d,%d,%d\n",offset2L,candidate,revoffset2R-cR));
	      
	      Intlist_free(&hits);
	      return make_microexon_pairs_double(offset1,offset1+cL,offset1+cL+middlelength,
						 offset2L,candidate,revoffset2R-cR+1,
						 cL,middlelength,cR,queryseq,genomicseg,
						 pairpool,ngap,gapchar);
	    }
	  }
	  Intlist_free(&hits);
	}
      }
    }
  }
  *microintrontype = NONINTRON;
  return NULL;
}


/* Based on probability of seeing a pattern of length n in L is
   1-(1-p1)^L, where p1 is 4^n.  We determine L so chance probability
   is less than ENDSEQUENCE_PVALUE */
static int
search_length (int endlength, int maxlength) {
  double p1;
  int result;

  if (endlength > 8) {
    return maxlength;
  } else {
    p1 = 1.0/pow(4.0,(double) (endlength+4));
    result = (int) (log(1.0-ENDSEQUENCE_PVALUE)/log(1-p1));
    debug(printf("  Search length for endlength of %d is %d\n",endlength,result));
    if (result > maxlength) {
      return maxlength;
    } else {
      return result;
    }
  }
}


List_T
Dynprog_microexon_5 (int *microintrontype, int *microexonlength, char *revsequence1, char *revsequence2,
		     int length1, int length2, int revoffset1, int revoffset2,
		     int cdna_direction, char *queryseq, char *genomicseg, 
		     Pairpool_T pairpool, int ngap) {
  Intlist_T hits = NULL, p;
  int endlength, c, textleft, textright, candidate, nmismatches = 0;
  char left1, left2, right2, right1;
  char intron1, intron2, intron3, intron4, gapchar;

  if (cdna_direction > 0) {
    intron1 = 'G';
    intron2 = 'T';
    intron3 = 'A';
    intron4 = 'G';
    gapchar = '>';
    *microintrontype = GTAG_FWD;
  } else if (cdna_direction < 0) {
    intron1 = 'C';
    intron2 = 'T';
    intron3 = 'A';
    intron4 = 'C';
    gapchar = '<';
    *microintrontype = GTAG_REV;
  } else {
    abort();
  }

  debug(printf("Begin microexon search at 5' for %.*s\n",
	       length2,&(revsequence2[-length2+1])));
  debug(printf("  Query sequence is %.*s\n",length1,&(revsequence1[-length1+1])));

  *microexonlength = 0;
  for (c = 0; c < length1 - MIN_MICROEXON_LENGTH; c++) {
    right2 = revsequence2[-c-1];
    right1 = revsequence2[-c];
    debug(printf("   Checking %c%c\n",right2,right1));
    if (c > 0 && revsequence1[-c+1] != revsequence2[-c+1]) {
      nmismatches++;
    }
    if (nmismatches > 1) {
      debug(printf("   Aborting at %c != %c\n",revsequence1[-c+1],revsequence2[-c+1]));
      *microintrontype = NONINTRON;
      return NULL;
    }
    if (right2 == intron3 && right1 == intron4) {
      endlength = length1 - c;
      debug(printf("  Found acceptor at %d, length %d.  End sequence is %.*s\n",
		       c,endlength,endlength,&(revsequence1[-endlength+1])));

      textright = revoffset2 - c - MICROINTRON_LENGTH;
      textleft = textright - search_length(endlength,textright) + MICROINTRON_LENGTH;
      hits = BoyerMoore(&(revsequence1[-c-endlength+1]),endlength,&(genomicseg[textleft]),textright-textleft);
      for (p = hits; p != NULL; p = Intlist_next(p)) {
	candidate = textleft + Intlist_head(p);
	if (genomicseg[candidate + endlength] == intron1 &&
	    genomicseg[candidate + endlength + 1] == intron2) {
	  debug(printf("  Successful microexon at %d\n",candidate));

	  Intlist_free(&hits);
	  *microexonlength = endlength;
	  return make_microexon_pairs_single(revoffset1-c-endlength+1,revoffset1-c+1,
					     candidate,revoffset2-c+1,
					     endlength,c,queryseq,genomicseg,
					     pairpool,ngap,gapchar);
	}
      }
      
      Intlist_free(&hits);
    }
  }
  *microintrontype = NONINTRON;
  return NULL;
}


List_T
Dynprog_microexon_3 (int *microintrontype, int *microexonlength, char *sequence1, char *sequence2,
		     int length1, int length2, int offset1, int offset2,
		     int cdna_direction, char *queryseq, char *genomicseg, 
		     int genomiclength, Pairpool_T pairpool, int ngap) {
  Intlist_T hits = NULL, p;
  int endlength, c, textleft, textright, candidate, nmismatches = 0;
  char left1, left2, right2, right1;
  char intron1, intron2, intron3, intron4, gapchar;

  if (cdna_direction > 0) {
    intron1 = 'G';
    intron2 = 'T';
    intron3 = 'A';
    intron4 = 'G';
    gapchar = '>';
    *microintrontype = GTAG_FWD;
  } else if (cdna_direction < 0) {
    intron1 = 'C';
    intron2 = 'T';
    intron3 = 'A';
    intron4 = 'C';
    gapchar = '<';
    *microintrontype = GTAG_REV;
  } else {
    abort();
  }

  debug(printf("Begin microexon search at 3' for %.*s\n",length2,sequence2));
  debug(printf("  Query sequence is %.*s\n",length1,sequence1));

  *microexonlength = 0;
  for (c = 0; c < length1 - MIN_MICROEXON_LENGTH; c++) {
    left1 = sequence2[c];
    left2 = sequence2[c+1];
    debug(printf("   Checking %c%c\n",left1,left2));
    if (c > 0 && sequence1[c-1] != sequence2[c-1]) {
      nmismatches++;
    }
    if (nmismatches > 1) {
      debug(printf("   Aborting at %c != %c\n",sequence1[c-1],sequence2[c-1]));
      *microintrontype = NONINTRON;
      return NULL;
    }
    if (left1 == intron1 && left2 == intron2) {
      endlength = length1 - c;
      debug(printf("  Found donor at %d, length %d.  End sequence is %.*s\n",
		   c,endlength,endlength,&(sequence1[c])));

      textleft = offset2 + c;
      textright = textleft + search_length(endlength,genomiclength-textleft);
      hits = BoyerMoore(&(sequence1[c]),endlength,&(genomicseg[textleft]),textright-textleft);
      for (p = hits; p != NULL; p = Intlist_next(p)) {
	candidate = textleft + Intlist_head(p);
	if (genomicseg[candidate - 2] == intron3 &&
	    genomicseg[candidate - 1] == intron4) {
	  debug(printf("  Successful microexon at %d\n",candidate));

	  Intlist_free(&hits);
	  *microexonlength = endlength;
	  return make_microexon_pairs_single(offset1,offset1+c,
					     offset2,candidate,
					     c,endlength,queryseq,genomicseg,
					     pairpool,ngap,gapchar);
	}
      }
      
      Intlist_free(&hits);
    }
  }
  *microintrontype = NONINTRON;
  return NULL;
}