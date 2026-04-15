/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Author: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * homography.h
 *
 * 4-point 2D homography computation using the Direct Linear Transform.
 *
 * gaussian_elimination implements the pseudocode from:
 *   https://en.wikipedia.org/wiki/Gaussian_elimination
 * with standard upper-triangular back-substitution.
 *
 * findHomography sets up the DLT system as described in:
 *   Hartley R. and Zisserman A., "Multiple View Geometry
 *   in Computer Vision", 2nd edition, Chapter 4.
 *   https://visionbook.mit.edu/homography.html
 */

#ifndef XJ_HOMOGRAPHY_H
#define XJ_HOMOGRAPHY_H

#ifdef HAVE_GL

#include <GL/gl.h>
#include <math.h>

typedef struct {
	double x;
	double y;
} Point;

/*
 * Solve a linear system represented as an m x n augmented matrix
 * using Gaussian elimination with partial pivoting, producing an
 * upper triangular matrix, then back-substitution to extract the
 * solution in the last column.
 *
 * m = n - 1 (n columns = m unknowns + 1 RHS column).
 *
 * Algorithm follows the Wikipedia pseudocode for row reduction
 * with partial pivoting (no pivot row scaling — the diagonal
 * retains its original pivoted value, and back-substitution
 * divides by it).
 */
void gaussian_elimination(double *input, int n) {
	double *A = input;
	int m = n - 1;  /* number of rows */
	int h = 0;      /* pivot row index */
	int k = 0;      /* pivot column index */

	/* Forward elimination (Wikipedia pseudocode) */
	while (h < m && k < n) {
		/* Find the row with the largest absolute value in column k */
		int i_max = h;
		int i;
		for (i = h + 1; i < m; i++) {
			if (fabs(A[i * n + k]) > fabs(A[i_max * n + k])) {
				i_max = i;
			}
		}

		if (A[i_max * n + k] == 0) {
			/* No pivot in this column, pass to next column */
			k++;
		} else {
			/* Swap rows h and i_max */
			if (h != i_max) {
				int j;
				for (j = 0; j < n; j++) {
					double tmp = A[h * n + j];
					A[h * n + j] = A[i_max * n + j];
					A[i_max * n + j] = tmp;
				}
			}

			/* Eliminate all rows below the pivot */
			for (i = h + 1; i < m; i++) {
				double f = A[i * n + k] / A[h * n + k];
				A[i * n + k] = 0;
				int j;
				for (j = k + 1; j < n; j++) {
					A[i * n + j] = A[i * n + j] - A[h * n + j] * f;
				}
			}

			h++;
			k++;
		}
	}

	/*
	 * Back-substitution for upper triangular system.
	 * The solution for each unknown x[i] is:
	 *   x[i] = (b[i] - sum(j=i+1..m-1, A[i,j]*x[j])) / A[i,i]
	 * where b[i] is the last column (RHS) of the augmented matrix.
	 */
	{
		int i;
		for (i = m - 1; i >= 0; i--) {
			int j;
			for (j = i + 1; j < m; j++) {
				A[i * n + m] -= A[i * n + j] * A[j * n + m];
			}
			A[i * n + m] /= A[i * n + i];
		}
	}
}

/*
 * Compute a 4x4 homography matrix that maps four source points
 * to four destination points. The result is stored in column-major
 * order suitable for use with glMultMatrixf().
 *
 * The DLT equations for each point correspondence (x,y) -> (x',y'):
 *   x' * (g*x + h*y + 1) = a*x + b*y + c
 *   y' * (g*x + h*y + 1) = d*x + e*y + f
 *
 * With the scale factor fixed at 1 (8 DOF), four point pairs give
 * 8 equations for 8 unknowns, solved as an 8x9 augmented system.
 */
void findHomography(Point src[4], Point dst[4], GLfloat homography[16]) {
	/* Build the 8x9 augmented matrix for the DLT system */
	double P[8][9] = {
		{-src[0].x, -src[0].y, -1,   0,         0,        0, src[0].x * dst[0].x, src[0].y * dst[0].x, -dst[0].x},
		{  0,        0,         0, -src[0].x, -src[0].y, -1, src[0].x * dst[0].y, src[0].y * dst[0].y, -dst[0].y},

		{-src[1].x, -src[1].y, -1,   0,         0,        0, src[1].x * dst[1].x, src[1].y * dst[1].x, -dst[1].x},
		{  0,        0,         0, -src[1].x, -src[1].y, -1, src[1].x * dst[1].y, src[1].y * dst[1].y, -dst[1].y},

		{-src[2].x, -src[2].y, -1,   0,         0,        0, src[2].x * dst[2].x, src[2].y * dst[2].x, -dst[2].x},
		{  0,        0,         0, -src[2].x, -src[2].y, -1, src[2].x * dst[2].y, src[2].y * dst[2].y, -dst[2].y},

		{-src[3].x, -src[3].y, -1,   0,         0,        0, src[3].x * dst[3].x, src[3].y * dst[3].x, -dst[3].x},
		{  0,        0,         0, -src[3].x, -src[3].y, -1, src[3].x * dst[3].y, src[3].y * dst[3].y, -dst[3].y},
	};

	gaussian_elimination(&P[0][0], 9);

	/*
	 * Assemble the 4x4 matrix in column-major order for OpenGL.
	 * The 2D homography maps (x, y) -> (x', y') with h33 = 1.
	 * Embedding into 4x4: z row/col are zero, w row/col carry
	 * the projective terms.
	 *
	 * Column-major layout:
	 *   col0: h11, h21, 0, h31
	 *   col1: h12, h22, 0, h32
	 *   col2:   0,   0, 0,   0
	 *   col3: h13, h23, 0,   1  (h33)
	 */
	double H[16] = {
		P[0][8], P[3][8], 0, P[6][8],
		P[1][8], P[4][8], 0, P[7][8],
		0,       0,       0, 0,
		P[2][8], P[5][8], 0, 1
	};

	int i;
	for (i = 0; i < 16; i++) {
		homography[i] = (GLfloat)H[i];
	}
}

#endif /* HAVE_GL */
#endif /* XJ_HOMOGRAPHY_H */
