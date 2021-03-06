/** @file
 * @brief This file contains all functions for the mutation matrix and the
 * estimation of evolutionary distances thereof.
 */

#include "model.h"
#include "global.h"
#include <gsl/gsl_randist.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

/**
 * @brief Sum some mutation count specified by `summands`. Intended to be used
 * through the `model_sum` macro.
 *
 * @param MM - The mutation matrix.
 * @param summands - The mutations to add.
 * @returns The sum of mutations.
 */
static size_t model_sum_types(const model *MM, const int summands[]) {
	size_t total = 0;
	for (int i = 0; summands[i] != MUTCOUNTS; ++i) {
		total += MM->counts[summands[i]];
	}
	return total;
}

#define model_sum(MM, ...)                                                     \
	model_sum_types((MM), (int[]){__VA_ARGS__, MUTCOUNTS})

/**
 * @brief Average two mutation matrices.
 *
 * @param MM - One matrix
 * @param NN - Second matrix
 * @returns The average (sum) of two mutation matrices.
 */
model model_average(const model *MM, const model *NN) {
	model ret = *MM;
	for (int i = 0; i != MUTCOUNTS; ++i) {
		ret.counts[i] += NN->counts[i];
	}
	ret.seq_len += NN->seq_len;
	return ret;
}

/**
 * @brief Compute the total number of nucleotides in the pairwise alignment.
 *
 * @param MM - The mutation matrix.
 * @returns The length of the alignment.
 */
size_t model_total(const model *MM) {
	size_t total = 0;
	for (size_t i = 0; i < MUTCOUNTS; ++i) {
		total += MM->counts[i];
	}
	return total;
}

/**
 * @brief Compute the coverage of an alignment.
 *
 * @param MM - The mutation matrix.
 * @returns The relative coverage
 */
double model_coverage(const model *MM) {
	size_t covered = model_total(MM);
	size_t actual = MM->seq_len;

	return (double)covered / (double)actual;
}

/**
 * @brief Estimate the uncorrected distance of a pairwise alignment.
 *
 * @param MM - The mutation matrix.
 * @returns The uncorrected substitution rate.
 */
double estimate_RAW(const model *MM) {
	size_t nucl = model_total(MM);
	size_t SNPs = model_sum(MM, AtoC, AtoG, AtoT, CtoG, CtoT, GtoT);

	// Insignificant results. All abort the fail train.
	if (nucl <= 3) {
		return NAN;
	}

	return (double)SNPs / (double)nucl;
}

/**
 * @brief Compute the Jukes-Cantor distance.
 *
 * @param MM - The mutation matrix.
 * @returns The corrected JC distance.
 */
double estimate_JC(const model *MM) {
	double dist = estimate_RAW(MM);
	dist = -0.75 * log(1.0 - (4.0 / 3.0) * dist); // jukes cantor

	// fix negative zero
	return dist <= 0.0 ? 0.0 : dist;
}

/** @brief computes the evolutionary distance using K80.
 *
 * @param MM - The mutation matrix.
 * @returns The corrected Kimura distance.
 */
double estimate_KIMURA(const model *MM) {
	size_t nucl = model_total(MM);
	size_t transitions = model_sum(MM, AtoG, CtoT);
	size_t transversions = model_sum(MM, AtoC, AtoT, GtoC, GtoT);

	double P = (double)transitions / (double)nucl;
	double Q = (double)transversions / (double)nucl;

	double tmp = 1.0 - 2.0 * P - Q;
	double dist = -0.25 * log((1.0 - 2.0 * Q) * tmp * tmp);

	// fix negative zero
	return dist <= 0.0 ? 0.0 : dist;
}

/** @brief Bootstrap a mutation matrix.
 *
 * The classical bootstrapping process, as described by Felsenstein, resamples
 * all nucleotides of a MSA. As andi only computes a pairwise alignment, this
 * process boils down to a simple multinomial distribution. We just have to
 * resample the elements of the mutation matrix. See Klötzl & Haubold (2016)
 * for details. http://www.mdpi.com/2075-1729/6/1/11/htm
 *
 * @param MM - The original mutation matrix.
 * @returns A bootstrapped mutation matrix.
 */
model model_bootstrap(const model MM) {
	model datum = MM;

	size_t nucl = model_total(&MM);
	double p[MUTCOUNTS];
	for (size_t i = 0; i < MUTCOUNTS; ++i) {
		p[i] = MM.counts[i] / (double)nucl;
	}

	unsigned int n[MUTCOUNTS];

	gsl_ran_multinomial(RNG, MUTCOUNTS, nucl, p, n);

	for (size_t i = 0; i < MUTCOUNTS; ++i) {
		datum.counts[i] = n[i];
	}

	return datum;
}

/**
 * @brief Given an anchor, classify nucleotides.
 *
 * For anchors we already know that the nucleotides of the subject and the query
 * are equal. Thus only one sequence has to be analysed. Most models don't
 * actually care about the individual nucleotides as long as they are equal in
 * the two sequences. For these models, we just assume equal distribution.
 *
 * @param MM - The mutation matrix
 * @param S - The subject
 * @param len - The anchor length
 */
void model_count_equal(model *MM, const char *S, size_t len) {
	if (MODEL == M_RAW || MODEL == M_JC || MODEL == M_KIMURA) {
		size_t fourth = len / 4;
		MM->counts[AtoA] += fourth;
		MM->counts[CtoC] += fourth;
		MM->counts[GtoG] += fourth;
		MM->counts[TtoT] += fourth + (len & 3);
		return;
	}

	// Fall-back algorithm for future models. Note, as this works on a
	// per-character basis it is slow.

	size_t local_counts[4] = {0};

	for (; len--;) {
		char s = *S++;

		// ';!#' are all smaller than 'A'
		if (s < 'A') {
			// Technically, s can only be ';' at this point.
			continue;
		}

		// The four canonical nucleotides can be uniquely identified by the bits
		// 0x6: A -> 0, C → 1, G → 3, T → 2. Thus the order below is changed.
		local_counts[(s >> 1) & 3]++;
	}

	MM->counts[AtoA] += local_counts[0];
	MM->counts[CtoC] += local_counts[1];
	MM->counts[GtoG] += local_counts[3];
	MM->counts[TtoT] += local_counts[2];
}

/** @brief Convert a nucleotide to a 2bit representation.
 *
 * We want to map characters:
 *  A → 0
 *  C → 1
 *  G → 2
 *  T → 3
 * The trick used below is that the three lower bits of the
 * characters are unique. Thus, they can be used to compute the mapping
 * above. The mapping itself is done via tricky bitwise operations.
 *
 * @param c - input nucleotide
 * @returns 2bit representation.
 */
char nucl2bit(char c) {
	c &= 6;
	c ^= c >> 1;
	return c >> 1;
}

/**
 * @brief Count the substitutions and add them to the mutation matrix.
 *
 * @param MM - The mutation matrix.
 * @param S - The subject
 * @param Q - The query
 * @param len - The length of the alignment
 */
void model_count(model *MM, const char *S, const char *Q, size_t len) {
	size_t local_counts[MUTCOUNTS] = {0};

	for (size_t i = 0; i < len; S++, Q++, i++) {
		char s = *S;
		char q = *Q;

		// Skip special characters.
		if (s < 'A' || q < 'A') {
			continue;
		}

		// Pick the correct two bits representing s and q.
		unsigned char foo = nucl2bit(s);
		unsigned char baz = nucl2bit(q);

		/*
		 * The mutation matrix is symmetric. For convenience we define the
		 * mutation XtoY with X > Y.
		 */
		if (baz > foo) {
			int temp = foo;
			foo = baz;
			baz = temp;
		}

		/*
		 * Finally, we want to map the indices to the correct mutation. This is
		 * done by utilising the mutation types in model.h.
		 */
		static const unsigned int map4 = 0x9740;
		unsigned int base = (map4 >> (4 * baz)) & 0xf;
		unsigned int index = base + (foo - baz);

		local_counts[index]++;
	}

	for (int i = 0; i != MUTCOUNTS; ++i) {
		MM->counts[i] += local_counts[i];
	}
}
