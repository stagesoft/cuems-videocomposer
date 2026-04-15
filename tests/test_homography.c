/*
 * test_homography.c — standalone unit tests for homography.h
 *
 * Compile: gcc -o test_homography test_homography.c -lm
 * Run:     ./test_homography
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

/* Mock GL types so we can compile without OpenGL headers */
#ifndef __gl_h_
#define __gl_h_
typedef float GLfloat;
#endif

#define HAVE_GL
#include "../src/cuems_videocomposer/homography.h"

#define EPSILON 1e-4  /* GLfloat is single-precision; large coords need wider tolerance */

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_NEAR(a, b, eps, msg) do { \
	if (fabs((a) - (b)) > (eps)) { \
		fprintf(stderr, "FAIL: %s: expected %.10f, got %.10f (diff %.2e)\n", \
			msg, (double)(b), (double)(a), fabs((double)(a) - (double)(b))); \
		tests_failed++; \
		return; \
	} \
} while(0)

/*
 * Apply a column-major 4x4 homography matrix to a 2D point.
 * Input point is treated as (x, y, 0, 1) in homogeneous coords.
 * Returns the dehomogenized (x'/w, y'/w) result.
 */
static Point apply_homography(GLfloat m[16], Point p) {
	/* Column-major: m[col*4 + row] */
	double x = m[0] * p.x + m[4] * p.y + m[12];  /* col0*x + col1*y + col3 */
	double y = m[1] * p.x + m[5] * p.y + m[13];
	double w = m[3] * p.x + m[7] * p.y + m[15];
	Point result;
	result.x = x / w;
	result.y = y / w;
	return result;
}

/* ------------------------------------------------------------------ */
/* Test: Gaussian elimination with a known 3x3 system                 */
/*   2x + y - z = 8                                                   */
/*  -3x - y + 2z = -11                                                */
/*  -2x + y + 2z = -3                                                 */
/*  Solution: x=2, y=3, z=-1                                          */
/* ------------------------------------------------------------------ */
static void test_gaussian_elimination(void) {
	printf("  test_gaussian_elimination... ");

	/* 3x4 augmented matrix (3 equations, 3 unknowns + RHS) */
	double A[3][4] = {
		{ 2,  1, -1,   8},
		{-3, -1,  2, -11},
		{-2,  1,  2,  -3}
	};

	gaussian_elimination(&A[0][0], 4);

	ASSERT_NEAR(A[0][3], 2.0, EPSILON, "x");
	ASSERT_NEAR(A[1][3], 3.0, EPSILON, "y");
	ASSERT_NEAR(A[2][3], -1.0, EPSILON, "z");

	tests_passed++;
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* Test: Identity homography (src == dst)                             */
/* ------------------------------------------------------------------ */
static void test_identity_homography(void) {
	printf("  test_identity_homography... ");

	Point src[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	Point dst[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	GLfloat H[16];

	findHomography(src, dst, H);

	/* Expected: identity matrix */
	GLfloat identity[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 1
	};

	/* Row 2 / col 2 are zero (z is unused in 2D homography) */
	int i;
	for (i = 0; i < 16; i++) {
		ASSERT_NEAR(H[i], identity[i], EPSILON, "identity element");
	}

	tests_passed++;
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* Test: Pure translation                                             */
/* ------------------------------------------------------------------ */
static void test_translation_homography(void) {
	printf("  test_translation_homography... ");

	double tx = 3.0, ty = -2.0;
	Point src[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	Point dst[4] = {
		{src[0].x + tx, src[0].y + ty},
		{src[1].x + tx, src[1].y + ty},
		{src[2].x + tx, src[2].y + ty},
		{src[3].x + tx, src[3].y + ty}
	};
	GLfloat H[16];

	findHomography(src, dst, H);

	/* Verify round-trip: apply H to each src point, should get dst */
	int i;
	for (i = 0; i < 4; i++) {
		Point result = apply_homography(H, src[i]);
		char msg[64];
		snprintf(msg, sizeof(msg), "translation point %d x", i);
		ASSERT_NEAR(result.x, dst[i].x, EPSILON, msg);
		snprintf(msg, sizeof(msg), "translation point %d y", i);
		ASSERT_NEAR(result.y, dst[i].y, EPSILON, msg);
	}

	tests_passed++;
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* Test: Pure scaling                                                 */
/* ------------------------------------------------------------------ */
static void test_scaling_homography(void) {
	printf("  test_scaling_homography... ");

	double scale = 2.5;
	Point src[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	Point dst[4] = {
		{src[0].x * scale, src[0].y * scale},
		{src[1].x * scale, src[1].y * scale},
		{src[2].x * scale, src[2].y * scale},
		{src[3].x * scale, src[3].y * scale}
	};
	GLfloat H[16];

	findHomography(src, dst, H);

	int i;
	for (i = 0; i < 4; i++) {
		Point result = apply_homography(H, src[i]);
		char msg[64];
		snprintf(msg, sizeof(msg), "scaling point %d x", i);
		ASSERT_NEAR(result.x, dst[i].x, EPSILON, msg);
		snprintf(msg, sizeof(msg), "scaling point %d y", i);
		ASSERT_NEAR(result.y, dst[i].y, EPSILON, msg);
	}

	tests_passed++;
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* Test: Asymmetric perspective warp (round-trip verification)        */
/* ------------------------------------------------------------------ */
static void test_perspective_homography(void) {
	printf("  test_perspective_homography... ");

	Point src[4] = {{-100, -100}, {100, -100}, {100, 100}, {-100, 100}};
	/* Arbitrary asymmetric corner offsets (simulates real corner deformation) */
	Point dst[4] = {
		{-90,  -105},   /* top-left shifted right and down */
		{ 110, -95},    /* top-right shifted right and up */
		{ 95,   110},   /* bottom-right shifted left and down */
		{-105,  90}     /* bottom-left shifted left and up */
	};
	GLfloat H[16];

	findHomography(src, dst, H);

	int i;
	for (i = 0; i < 4; i++) {
		Point result = apply_homography(H, src[i]);
		char msg[64];
		snprintf(msg, sizeof(msg), "perspective point %d x", i);
		ASSERT_NEAR(result.x, dst[i].x, EPSILON, msg);
		snprintf(msg, sizeof(msg), "perspective point %d y", i);
		ASSERT_NEAR(result.y, dst[i].y, EPSILON, msg);
	}

	tests_passed++;
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* Test: Typical videocomposer usage pattern                          */
/* (quad centered at origin with corner deformation offsets)          */
/* ------------------------------------------------------------------ */
static void test_videocomposer_pattern(void) {
	printf("  test_videocomposer_pattern... ");

	double quad_x = 960.0, quad_y = 540.0;
	/* Corner deformation offsets as used in display_gl_common.h */
	double offsets[8] = {10, -5, -8, 3, 12, 7, -4, -10};

	Point src[4] = {
		{-quad_x, -quad_y},
		{ quad_x, -quad_y},
		{ quad_x,  quad_y},
		{-quad_x,  quad_y}
	};
	Point dst[4];
	int i;
	for (i = 0; i < 4; i++) {
		dst[i].x = src[i].x + offsets[i * 2];
		dst[i].y = src[i].y + offsets[i * 2 + 1];
	}

	GLfloat H[16];
	findHomography(src, dst, H);

	for (i = 0; i < 4; i++) {
		Point result = apply_homography(H, src[i]);
		char msg[64];
		snprintf(msg, sizeof(msg), "videocomposer point %d x", i);
		ASSERT_NEAR(result.x, dst[i].x, EPSILON, msg);
		snprintf(msg, sizeof(msg), "videocomposer point %d y", i);
		ASSERT_NEAR(result.y, dst[i].y, EPSILON, msg);
	}

	tests_passed++;
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
int main(void) {
	printf("Running homography tests...\n\n");

	test_gaussian_elimination();
	test_identity_homography();
	test_translation_homography();
	test_scaling_homography();
	test_perspective_homography();
	test_videocomposer_pattern();

	printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
	return tests_failed > 0 ? 1 : 0;
}
