#include "isl_mat.h"
#include "isl_map_private.h"
#include "isl_tab.h"
#include "isl_seq.h"

/*
 * The implementation of tableaus in this file was inspired by Section 8
 * of David Detlefs, Greg Nelson and James B. Saxe, "Simplify: a theorem
 * prover for program checking".
 */

struct isl_tab *isl_tab_alloc(struct isl_ctx *ctx,
	unsigned n_row, unsigned n_var, unsigned M)
{
	int i;
	struct isl_tab *tab;
	unsigned off = 2 + M;

	tab = isl_calloc_type(ctx, struct isl_tab);
	if (!tab)
		return NULL;
	tab->mat = isl_mat_alloc(ctx, n_row, off + n_var);
	if (!tab->mat)
		goto error;
	tab->var = isl_alloc_array(ctx, struct isl_tab_var, n_var);
	if (!tab->var)
		goto error;
	tab->con = isl_alloc_array(ctx, struct isl_tab_var, n_row);
	if (!tab->con)
		goto error;
	tab->col_var = isl_alloc_array(ctx, int, n_var);
	if (!tab->col_var)
		goto error;
	tab->row_var = isl_alloc_array(ctx, int, n_row);
	if (!tab->row_var)
		goto error;
	for (i = 0; i < n_var; ++i) {
		tab->var[i].index = i;
		tab->var[i].is_row = 0;
		tab->var[i].is_nonneg = 0;
		tab->var[i].is_zero = 0;
		tab->var[i].is_redundant = 0;
		tab->var[i].frozen = 0;
		tab->var[i].negated = 0;
		tab->col_var[i] = i;
	}
	tab->n_row = 0;
	tab->n_con = 0;
	tab->n_eq = 0;
	tab->max_con = n_row;
	tab->n_col = n_var;
	tab->n_var = n_var;
	tab->max_var = n_var;
	tab->n_param = 0;
	tab->n_div = 0;
	tab->n_dead = 0;
	tab->n_redundant = 0;
	tab->need_undo = 0;
	tab->rational = 0;
	tab->empty = 0;
	tab->in_undo = 0;
	tab->M = M;
	tab->cone = 0;
	tab->bottom.type = isl_tab_undo_bottom;
	tab->bottom.next = NULL;
	tab->top = &tab->bottom;

	tab->n_zero = 0;
	tab->n_unbounded = 0;
	tab->basis = NULL;

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

int isl_tab_extend_cons(struct isl_tab *tab, unsigned n_new)
{
	unsigned off = 2 + tab->M;

	if (!tab)
		return -1;

	if (tab->max_con < tab->n_con + n_new) {
		struct isl_tab_var *con;

		con = isl_realloc_array(tab->mat->ctx, tab->con,
				    struct isl_tab_var, tab->max_con + n_new);
		if (!con)
			return -1;
		tab->con = con;
		tab->max_con += n_new;
	}
	if (tab->mat->n_row < tab->n_row + n_new) {
		int *row_var;

		tab->mat = isl_mat_extend(tab->mat,
					tab->n_row + n_new, off + tab->n_col);
		if (!tab->mat)
			return -1;
		row_var = isl_realloc_array(tab->mat->ctx, tab->row_var,
					    int, tab->mat->n_row);
		if (!row_var)
			return -1;
		tab->row_var = row_var;
		if (tab->row_sign) {
			enum isl_tab_row_sign *s;
			s = isl_realloc_array(tab->mat->ctx, tab->row_sign,
					enum isl_tab_row_sign, tab->mat->n_row);
			if (!s)
				return -1;
			tab->row_sign = s;
		}
	}
	return 0;
}

/* Make room for at least n_new extra variables.
 * Return -1 if anything went wrong.
 */
int isl_tab_extend_vars(struct isl_tab *tab, unsigned n_new)
{
	struct isl_tab_var *var;
	unsigned off = 2 + tab->M;

	if (tab->max_var < tab->n_var + n_new) {
		var = isl_realloc_array(tab->mat->ctx, tab->var,
				    struct isl_tab_var, tab->n_var + n_new);
		if (!var)
			return -1;
		tab->var = var;
		tab->max_var += n_new;
	}

	if (tab->mat->n_col < off + tab->n_col + n_new) {
		int *p;

		tab->mat = isl_mat_extend(tab->mat,
				    tab->mat->n_row, off + tab->n_col + n_new);
		if (!tab->mat)
			return -1;
		p = isl_realloc_array(tab->mat->ctx, tab->col_var,
					    int, tab->n_col + n_new);
		if (!p)
			return -1;
		tab->col_var = p;
	}

	return 0;
}

struct isl_tab *isl_tab_extend(struct isl_tab *tab, unsigned n_new)
{
	if (isl_tab_extend_cons(tab, n_new) >= 0)
		return tab;

	isl_tab_free(tab);
	return NULL;
}

static void free_undo(struct isl_tab *tab)
{
	struct isl_tab_undo *undo, *next;

	for (undo = tab->top; undo && undo != &tab->bottom; undo = next) {
		next = undo->next;
		free(undo);
	}
	tab->top = undo;
}

void isl_tab_free(struct isl_tab *tab)
{
	if (!tab)
		return;
	free_undo(tab);
	isl_mat_free(tab->mat);
	isl_vec_free(tab->dual);
	isl_basic_set_free(tab->bset);
	free(tab->var);
	free(tab->con);
	free(tab->row_var);
	free(tab->col_var);
	free(tab->row_sign);
	isl_mat_free(tab->samples);
	free(tab->sample_index);
	isl_mat_free(tab->basis);
	free(tab);
}

struct isl_tab *isl_tab_dup(struct isl_tab *tab)
{
	int i;
	struct isl_tab *dup;
	unsigned off;

	if (!tab)
		return NULL;

	off = 2 + tab->M;
	dup = isl_calloc_type(tab->ctx, struct isl_tab);
	if (!dup)
		return NULL;
	dup->mat = isl_mat_dup(tab->mat);
	if (!dup->mat)
		goto error;
	dup->var = isl_alloc_array(tab->ctx, struct isl_tab_var, tab->max_var);
	if (!dup->var)
		goto error;
	for (i = 0; i < tab->n_var; ++i)
		dup->var[i] = tab->var[i];
	dup->con = isl_alloc_array(tab->ctx, struct isl_tab_var, tab->max_con);
	if (!dup->con)
		goto error;
	for (i = 0; i < tab->n_con; ++i)
		dup->con[i] = tab->con[i];
	dup->col_var = isl_alloc_array(tab->ctx, int, tab->mat->n_col - off);
	if (!dup->col_var)
		goto error;
	for (i = 0; i < tab->n_col; ++i)
		dup->col_var[i] = tab->col_var[i];
	dup->row_var = isl_alloc_array(tab->ctx, int, tab->mat->n_row);
	if (!dup->row_var)
		goto error;
	for (i = 0; i < tab->n_row; ++i)
		dup->row_var[i] = tab->row_var[i];
	if (tab->row_sign) {
		dup->row_sign = isl_alloc_array(tab->ctx, enum isl_tab_row_sign,
						tab->mat->n_row);
		if (!dup->row_sign)
			goto error;
		for (i = 0; i < tab->n_row; ++i)
			dup->row_sign[i] = tab->row_sign[i];
	}
	if (tab->samples) {
		dup->samples = isl_mat_dup(tab->samples);
		if (!dup->samples)
			goto error;
		dup->sample_index = isl_alloc_array(tab->mat->ctx, int,
							tab->samples->n_row);
		if (!dup->sample_index)
			goto error;
		dup->n_sample = tab->n_sample;
		dup->n_outside = tab->n_outside;
	}
	dup->n_row = tab->n_row;
	dup->n_con = tab->n_con;
	dup->n_eq = tab->n_eq;
	dup->max_con = tab->max_con;
	dup->n_col = tab->n_col;
	dup->n_var = tab->n_var;
	dup->max_var = tab->max_var;
	dup->n_param = tab->n_param;
	dup->n_div = tab->n_div;
	dup->n_dead = tab->n_dead;
	dup->n_redundant = tab->n_redundant;
	dup->rational = tab->rational;
	dup->empty = tab->empty;
	dup->need_undo = 0;
	dup->in_undo = 0;
	dup->M = tab->M;
	tab->cone = tab->cone;
	dup->bottom.type = isl_tab_undo_bottom;
	dup->bottom.next = NULL;
	dup->top = &dup->bottom;

	dup->n_zero = tab->n_zero;
	dup->n_unbounded = tab->n_unbounded;
	dup->basis = isl_mat_dup(tab->basis);

	return dup;
error:
	isl_tab_free(dup);
	return NULL;
}

/* Construct the coefficient matrix of the product tableau
 * of two tableaus.
 * mat{1,2} is the coefficient matrix of tableau {1,2}
 * row{1,2} is the number of rows in tableau {1,2}
 * col{1,2} is the number of columns in tableau {1,2}
 * off is the offset to the coefficient column (skipping the
 *	denominator, the constant term and the big parameter if any)
 * r{1,2} is the number of redundant rows in tableau {1,2}
 * d{1,2} is the number of dead columns in tableau {1,2}
 *
 * The order of the rows and columns in the result is as explained
 * in isl_tab_product.
 */
static struct isl_mat *tab_mat_product(struct isl_mat *mat1,
	struct isl_mat *mat2, unsigned row1, unsigned row2,
	unsigned col1, unsigned col2,
	unsigned off, unsigned r1, unsigned r2, unsigned d1, unsigned d2)
{
	int i;
	struct isl_mat *prod;
	unsigned n;

	prod = isl_mat_alloc(mat1->ctx, mat1->n_row + mat2->n_row,
					off + col1 + col2);

	n = 0;
	for (i = 0; i < r1; ++i) {
		isl_seq_cpy(prod->row[n + i], mat1->row[i], off + d1);
		isl_seq_clr(prod->row[n + i] + off + d1, d2);
		isl_seq_cpy(prod->row[n + i] + off + d1 + d2,
				mat1->row[i] + off + d1, col1 - d1);
		isl_seq_clr(prod->row[n + i] + off + col1 + d1, col2 - d2);
	}

	n += r1;
	for (i = 0; i < r2; ++i) {
		isl_seq_cpy(prod->row[n + i], mat2->row[i], off);
		isl_seq_clr(prod->row[n + i] + off, d1);
		isl_seq_cpy(prod->row[n + i] + off + d1,
			    mat2->row[i] + off, d2);
		isl_seq_clr(prod->row[n + i] + off + d1 + d2, col1 - d1);
		isl_seq_cpy(prod->row[n + i] + off + col1 + d1,
			    mat2->row[i] + off + d2, col2 - d2);
	}

	n += r2;
	for (i = 0; i < row1 - r1; ++i) {
		isl_seq_cpy(prod->row[n + i], mat1->row[r1 + i], off + d1);
		isl_seq_clr(prod->row[n + i] + off + d1, d2);
		isl_seq_cpy(prod->row[n + i] + off + d1 + d2,
				mat1->row[r1 + i] + off + d1, col1 - d1);
		isl_seq_clr(prod->row[n + i] + off + col1 + d1, col2 - d2);
	}

	n += row1 - r1;
	for (i = 0; i < row2 - r2; ++i) {
		isl_seq_cpy(prod->row[n + i], mat2->row[r2 + i], off);
		isl_seq_clr(prod->row[n + i] + off, d1);
		isl_seq_cpy(prod->row[n + i] + off + d1,
			    mat2->row[r2 + i] + off, d2);
		isl_seq_clr(prod->row[n + i] + off + d1 + d2, col1 - d1);
		isl_seq_cpy(prod->row[n + i] + off + col1 + d1,
			    mat2->row[r2 + i] + off + d2, col2 - d2);
	}

	return prod;
}

/* Update the row or column index of a variable that corresponds
 * to a variable in the first input tableau.
 */
static void update_index1(struct isl_tab_var *var,
	unsigned r1, unsigned r2, unsigned d1, unsigned d2)
{
	if (var->index == -1)
		return;
	if (var->is_row && var->index >= r1)
		var->index += r2;
	if (!var->is_row && var->index >= d1)
		var->index += d2;
}

/* Update the row or column index of a variable that corresponds
 * to a variable in the second input tableau.
 */
static void update_index2(struct isl_tab_var *var,
	unsigned row1, unsigned col1,
	unsigned r1, unsigned r2, unsigned d1, unsigned d2)
{
	if (var->index == -1)
		return;
	if (var->is_row) {
		if (var->index < r2)
			var->index += r1;
		else
			var->index += row1;
	} else {
		if (var->index < d2)
			var->index += d1;
		else
			var->index += col1;
	}
}

/* Create a tableau that represents the Cartesian product of the sets
 * represented by tableaus tab1 and tab2.
 * The order of the rows in the product is
 *	- redundant rows of tab1
 *	- redundant rows of tab2
 *	- non-redundant rows of tab1
 *	- non-redundant rows of tab2
 * The order of the columns is
 *	- denominator
 *	- constant term
 *	- coefficient of big parameter, if any
 *	- dead columns of tab1
 *	- dead columns of tab2
 *	- live columns of tab1
 *	- live columns of tab2
 * The order of the variables and the constraints is a concatenation
 * of order in the two input tableaus.
 */
struct isl_tab *isl_tab_product(struct isl_tab *tab1, struct isl_tab *tab2)
{
	int i;
	struct isl_tab *prod;
	unsigned off;
	unsigned r1, r2, d1, d2;

	if (!tab1 || !tab2)
		return NULL;

	isl_assert(tab1->mat->ctx, tab1->M == tab2->M, return NULL);
	isl_assert(tab1->mat->ctx, tab1->rational == tab2->rational, return NULL);
	isl_assert(tab1->mat->ctx, tab1->cone == tab2->cone, return NULL);
	isl_assert(tab1->mat->ctx, !tab1->row_sign, return NULL);
	isl_assert(tab1->mat->ctx, !tab2->row_sign, return NULL);
	isl_assert(tab1->mat->ctx, tab1->n_param == 0, return NULL);
	isl_assert(tab1->mat->ctx, tab2->n_param == 0, return NULL);
	isl_assert(tab1->mat->ctx, tab1->n_div == 0, return NULL);
	isl_assert(tab1->mat->ctx, tab2->n_div == 0, return NULL);

	off = 2 + tab1->M;
	r1 = tab1->n_redundant;
	r2 = tab2->n_redundant;
	d1 = tab1->n_dead;
	d2 = tab2->n_dead;
	prod = isl_calloc_type(tab1->mat->ctx, struct isl_tab);
	if (!prod)
		return NULL;
	prod->mat = tab_mat_product(tab1->mat, tab2->mat,
				tab1->n_row, tab2->n_row,
				tab1->n_col, tab2->n_col, off, r1, r2, d1, d2);
	if (!prod->mat)
		goto error;
	prod->var = isl_alloc_array(tab1->mat->ctx, struct isl_tab_var,
					tab1->max_var + tab2->max_var);
	if (!prod->var)
		goto error;
	for (i = 0; i < tab1->n_var; ++i) {
		prod->var[i] = tab1->var[i];
		update_index1(&prod->var[i], r1, r2, d1, d2);
	}
	for (i = 0; i < tab2->n_var; ++i) {
		prod->var[tab1->n_var + i] = tab2->var[i];
		update_index2(&prod->var[tab1->n_var + i],
				tab1->n_row, tab1->n_col,
				r1, r2, d1, d2);
	}
	prod->con = isl_alloc_array(tab1->mat->ctx, struct isl_tab_var,
					tab1->max_con +  tab2->max_con);
	if (!prod->con)
		goto error;
	for (i = 0; i < tab1->n_con; ++i) {
		prod->con[i] = tab1->con[i];
		update_index1(&prod->con[i], r1, r2, d1, d2);
	}
	for (i = 0; i < tab2->n_con; ++i) {
		prod->con[tab1->n_con + i] = tab2->con[i];
		update_index2(&prod->con[tab1->n_con + i],
				tab1->n_row, tab1->n_col,
				r1, r2, d1, d2);
	}
	prod->col_var = isl_alloc_array(tab1->mat->ctx, int,
					tab1->n_col + tab2->n_col);
	if (!prod->col_var)
		goto error;
	for (i = 0; i < tab1->n_col; ++i) {
		int pos = i < d1 ? i : i + d2;
		prod->col_var[pos] = tab1->col_var[i];
	}
	for (i = 0; i < tab2->n_col; ++i) {
		int pos = i < d2 ? d1 + i : tab1->n_col + i;
		int t = tab2->col_var[i];
		if (t >= 0)
			t += tab1->n_var;
		else
			t -= tab1->n_con;
		prod->col_var[pos] = t;
	}
	prod->row_var = isl_alloc_array(tab1->mat->ctx, int,
					tab1->mat->n_row + tab2->mat->n_row);
	if (!prod->row_var)
		goto error;
	for (i = 0; i < tab1->n_row; ++i) {
		int pos = i < r1 ? i : i + r2;
		prod->row_var[pos] = tab1->row_var[i];
	}
	for (i = 0; i < tab2->n_row; ++i) {
		int pos = i < r2 ? r1 + i : tab1->n_row + i;
		int t = tab2->row_var[i];
		if (t >= 0)
			t += tab1->n_var;
		else
			t -= tab1->n_con;
		prod->row_var[pos] = t;
	}
	prod->samples = NULL;
	prod->sample_index = NULL;
	prod->n_row = tab1->n_row + tab2->n_row;
	prod->n_con = tab1->n_con + tab2->n_con;
	prod->n_eq = 0;
	prod->max_con = tab1->max_con + tab2->max_con;
	prod->n_col = tab1->n_col + tab2->n_col;
	prod->n_var = tab1->n_var + tab2->n_var;
	prod->max_var = tab1->max_var + tab2->max_var;
	prod->n_param = 0;
	prod->n_div = 0;
	prod->n_dead = tab1->n_dead + tab2->n_dead;
	prod->n_redundant = tab1->n_redundant + tab2->n_redundant;
	prod->rational = tab1->rational;
	prod->empty = tab1->empty || tab2->empty;
	prod->need_undo = 0;
	prod->in_undo = 0;
	prod->M = tab1->M;
	prod->cone = tab1->cone;
	prod->bottom.type = isl_tab_undo_bottom;
	prod->bottom.next = NULL;
	prod->top = &prod->bottom;

	prod->n_zero = 0;
	prod->n_unbounded = 0;
	prod->basis = NULL;

	return prod;
error:
	isl_tab_free(prod);
	return NULL;
}

static struct isl_tab_var *var_from_index(struct isl_tab *tab, int i)
{
	if (i >= 0)
		return &tab->var[i];
	else
		return &tab->con[~i];
}

struct isl_tab_var *isl_tab_var_from_row(struct isl_tab *tab, int i)
{
	return var_from_index(tab, tab->row_var[i]);
}

static struct isl_tab_var *var_from_col(struct isl_tab *tab, int i)
{
	return var_from_index(tab, tab->col_var[i]);
}

/* Check if there are any upper bounds on column variable "var",
 * i.e., non-negative rows where var appears with a negative coefficient.
 * Return 1 if there are no such bounds.
 */
static int max_is_manifestly_unbounded(struct isl_tab *tab,
	struct isl_tab_var *var)
{
	int i;
	unsigned off = 2 + tab->M;

	if (var->is_row)
		return 0;
	for (i = tab->n_redundant; i < tab->n_row; ++i) {
		if (!isl_int_is_neg(tab->mat->row[i][off + var->index]))
			continue;
		if (isl_tab_var_from_row(tab, i)->is_nonneg)
			return 0;
	}
	return 1;
}

/* Check if there are any lower bounds on column variable "var",
 * i.e., non-negative rows where var appears with a positive coefficient.
 * Return 1 if there are no such bounds.
 */
static int min_is_manifestly_unbounded(struct isl_tab *tab,
	struct isl_tab_var *var)
{
	int i;
	unsigned off = 2 + tab->M;

	if (var->is_row)
		return 0;
	for (i = tab->n_redundant; i < tab->n_row; ++i) {
		if (!isl_int_is_pos(tab->mat->row[i][off + var->index]))
			continue;
		if (isl_tab_var_from_row(tab, i)->is_nonneg)
			return 0;
	}
	return 1;
}

static int row_cmp(struct isl_tab *tab, int r1, int r2, int c, isl_int t)
{
	unsigned off = 2 + tab->M;

	if (tab->M) {
		int s;
		isl_int_mul(t, tab->mat->row[r1][2], tab->mat->row[r2][off+c]);
		isl_int_submul(t, tab->mat->row[r2][2], tab->mat->row[r1][off+c]);
		s = isl_int_sgn(t);
		if (s)
			return s;
	}
	isl_int_mul(t, tab->mat->row[r1][1], tab->mat->row[r2][off + c]);
	isl_int_submul(t, tab->mat->row[r2][1], tab->mat->row[r1][off + c]);
	return isl_int_sgn(t);
}

/* Given the index of a column "c", return the index of a row
 * that can be used to pivot the column in, with either an increase
 * (sgn > 0) or a decrease (sgn < 0) of the corresponding variable.
 * If "var" is not NULL, then the row returned will be different from
 * the one associated with "var".
 *
 * Each row in the tableau is of the form
 *
 *	x_r = a_r0 + \sum_i a_ri x_i
 *
 * Only rows with x_r >= 0 and with the sign of a_ri opposite to "sgn"
 * impose any limit on the increase or decrease in the value of x_c
 * and this bound is equal to a_r0 / |a_rc|.  We are therefore looking
 * for the row with the smallest (most stringent) such bound.
 * Note that the common denominator of each row drops out of the fraction.
 * To check if row j has a smaller bound than row r, i.e.,
 * a_j0 / |a_jc| < a_r0 / |a_rc| or a_j0 |a_rc| < a_r0 |a_jc|,
 * we check if -sign(a_jc) (a_j0 a_rc - a_r0 a_jc) < 0,
 * where -sign(a_jc) is equal to "sgn".
 */
static int pivot_row(struct isl_tab *tab,
	struct isl_tab_var *var, int sgn, int c)
{
	int j, r, tsgn;
	isl_int t;
	unsigned off = 2 + tab->M;

	isl_int_init(t);
	r = -1;
	for (j = tab->n_redundant; j < tab->n_row; ++j) {
		if (var && j == var->index)
			continue;
		if (!isl_tab_var_from_row(tab, j)->is_nonneg)
			continue;
		if (sgn * isl_int_sgn(tab->mat->row[j][off + c]) >= 0)
			continue;
		if (r < 0) {
			r = j;
			continue;
		}
		tsgn = sgn * row_cmp(tab, r, j, c, t);
		if (tsgn < 0 || (tsgn == 0 &&
					    tab->row_var[j] < tab->row_var[r]))
			r = j;
	}
	isl_int_clear(t);
	return r;
}

/* Find a pivot (row and col) that will increase (sgn > 0) or decrease
 * (sgn < 0) the value of row variable var.
 * If not NULL, then skip_var is a row variable that should be ignored
 * while looking for a pivot row.  It is usually equal to var.
 *
 * As the given row in the tableau is of the form
 *
 *	x_r = a_r0 + \sum_i a_ri x_i
 *
 * we need to find a column such that the sign of a_ri is equal to "sgn"
 * (such that an increase in x_i will have the desired effect) or a
 * column with a variable that may attain negative values.
 * If a_ri is positive, then we need to move x_i in the same direction
 * to obtain the desired effect.  Otherwise, x_i has to move in the
 * opposite direction.
 */
static void find_pivot(struct isl_tab *tab,
	struct isl_tab_var *var, struct isl_tab_var *skip_var,
	int sgn, int *row, int *col)
{
	int j, r, c;
	isl_int *tr;

	*row = *col = -1;

	isl_assert(tab->mat->ctx, var->is_row, return);
	tr = tab->mat->row[var->index] + 2 + tab->M;

	c = -1;
	for (j = tab->n_dead; j < tab->n_col; ++j) {
		if (isl_int_is_zero(tr[j]))
			continue;
		if (isl_int_sgn(tr[j]) != sgn &&
		    var_from_col(tab, j)->is_nonneg)
			continue;
		if (c < 0 || tab->col_var[j] < tab->col_var[c])
			c = j;
	}
	if (c < 0)
		return;

	sgn *= isl_int_sgn(tr[c]);
	r = pivot_row(tab, skip_var, sgn, c);
	*row = r < 0 ? var->index : r;
	*col = c;
}

/* Return 1 if row "row" represents an obviously redundant inequality.
 * This means
 *	- it represents an inequality or a variable
 *	- that is the sum of a non-negative sample value and a positive
 *	  combination of zero or more non-negative constraints.
 */
int isl_tab_row_is_redundant(struct isl_tab *tab, int row)
{
	int i;
	unsigned off = 2 + tab->M;

	if (tab->row_var[row] < 0 && !isl_tab_var_from_row(tab, row)->is_nonneg)
		return 0;

	if (isl_int_is_neg(tab->mat->row[row][1]))
		return 0;
	if (tab->M && isl_int_is_neg(tab->mat->row[row][2]))
		return 0;

	for (i = tab->n_dead; i < tab->n_col; ++i) {
		if (isl_int_is_zero(tab->mat->row[row][off + i]))
			continue;
		if (tab->col_var[i] >= 0)
			return 0;
		if (isl_int_is_neg(tab->mat->row[row][off + i]))
			return 0;
		if (!var_from_col(tab, i)->is_nonneg)
			return 0;
	}
	return 1;
}

static void swap_rows(struct isl_tab *tab, int row1, int row2)
{
	int t;
	t = tab->row_var[row1];
	tab->row_var[row1] = tab->row_var[row2];
	tab->row_var[row2] = t;
	isl_tab_var_from_row(tab, row1)->index = row1;
	isl_tab_var_from_row(tab, row2)->index = row2;
	tab->mat = isl_mat_swap_rows(tab->mat, row1, row2);

	if (!tab->row_sign)
		return;
	t = tab->row_sign[row1];
	tab->row_sign[row1] = tab->row_sign[row2];
	tab->row_sign[row2] = t;
}

static int push_union(struct isl_tab *tab,
	enum isl_tab_undo_type type, union isl_tab_undo_val u) WARN_UNUSED;
static int push_union(struct isl_tab *tab,
	enum isl_tab_undo_type type, union isl_tab_undo_val u)
{
	struct isl_tab_undo *undo;

	if (!tab->need_undo)
		return 0;

	undo = isl_alloc_type(tab->mat->ctx, struct isl_tab_undo);
	if (!undo)
		return -1;
	undo->type = type;
	undo->u = u;
	undo->next = tab->top;
	tab->top = undo;

	return 0;
}

int isl_tab_push_var(struct isl_tab *tab,
	enum isl_tab_undo_type type, struct isl_tab_var *var)
{
	union isl_tab_undo_val u;
	if (var->is_row)
		u.var_index = tab->row_var[var->index];
	else
		u.var_index = tab->col_var[var->index];
	return push_union(tab, type, u);
}

int isl_tab_push(struct isl_tab *tab, enum isl_tab_undo_type type)
{
	union isl_tab_undo_val u = { 0 };
	return push_union(tab, type, u);
}

/* Push a record on the undo stack describing the current basic
 * variables, so that the this state can be restored during rollback.
 */
int isl_tab_push_basis(struct isl_tab *tab)
{
	int i;
	union isl_tab_undo_val u;

	u.col_var = isl_alloc_array(tab->mat->ctx, int, tab->n_col);
	if (!u.col_var)
		return -1;
	for (i = 0; i < tab->n_col; ++i)
		u.col_var[i] = tab->col_var[i];
	return push_union(tab, isl_tab_undo_saved_basis, u);
}

int isl_tab_push_callback(struct isl_tab *tab, struct isl_tab_callback *callback)
{
	union isl_tab_undo_val u;
	u.callback = callback;
	return push_union(tab, isl_tab_undo_callback, u);
}

struct isl_tab *isl_tab_init_samples(struct isl_tab *tab)
{
	if (!tab)
		return NULL;

	tab->n_sample = 0;
	tab->n_outside = 0;
	tab->samples = isl_mat_alloc(tab->mat->ctx, 1, 1 + tab->n_var);
	if (!tab->samples)
		goto error;
	tab->sample_index = isl_alloc_array(tab->mat->ctx, int, 1);
	if (!tab->sample_index)
		goto error;
	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

struct isl_tab *isl_tab_add_sample(struct isl_tab *tab,
	__isl_take isl_vec *sample)
{
	if (!tab || !sample)
		goto error;

	if (tab->n_sample + 1 > tab->samples->n_row) {
		int *t = isl_realloc_array(tab->mat->ctx,
			    tab->sample_index, int, tab->n_sample + 1);
		if (!t)
			goto error;
		tab->sample_index = t;
	}

	tab->samples = isl_mat_extend(tab->samples,
				tab->n_sample + 1, tab->samples->n_col);
	if (!tab->samples)
		goto error;

	isl_seq_cpy(tab->samples->row[tab->n_sample], sample->el, sample->size);
	isl_vec_free(sample);
	tab->sample_index[tab->n_sample] = tab->n_sample;
	tab->n_sample++;

	return tab;
error:
	isl_vec_free(sample);
	isl_tab_free(tab);
	return NULL;
}

struct isl_tab *isl_tab_drop_sample(struct isl_tab *tab, int s)
{
	if (s != tab->n_outside) {
		int t = tab->sample_index[tab->n_outside];
		tab->sample_index[tab->n_outside] = tab->sample_index[s];
		tab->sample_index[s] = t;
		isl_mat_swap_rows(tab->samples, tab->n_outside, s);
	}
	tab->n_outside++;
	if (isl_tab_push(tab, isl_tab_undo_drop_sample) < 0) {
		isl_tab_free(tab);
		return NULL;
	}

	return tab;
}

/* Record the current number of samples so that we can remove newer
 * samples during a rollback.
 */
int isl_tab_save_samples(struct isl_tab *tab)
{
	union isl_tab_undo_val u;

	if (!tab)
		return -1;

	u.n = tab->n_sample;
	return push_union(tab, isl_tab_undo_saved_samples, u);
}

/* Mark row with index "row" as being redundant.
 * If we may need to undo the operation or if the row represents
 * a variable of the original problem, the row is kept,
 * but no longer considered when looking for a pivot row.
 * Otherwise, the row is simply removed.
 *
 * The row may be interchanged with some other row.  If it
 * is interchanged with a later row, return 1.  Otherwise return 0.
 * If the rows are checked in order in the calling function,
 * then a return value of 1 means that the row with the given
 * row number may now contain a different row that hasn't been checked yet.
 */
int isl_tab_mark_redundant(struct isl_tab *tab, int row)
{
	struct isl_tab_var *var = isl_tab_var_from_row(tab, row);
	var->is_redundant = 1;
	isl_assert(tab->mat->ctx, row >= tab->n_redundant, return -1);
	if (tab->need_undo || tab->row_var[row] >= 0) {
		if (tab->row_var[row] >= 0 && !var->is_nonneg) {
			var->is_nonneg = 1;
			if (isl_tab_push_var(tab, isl_tab_undo_nonneg, var) < 0)
				return -1;
		}
		if (row != tab->n_redundant)
			swap_rows(tab, row, tab->n_redundant);
		tab->n_redundant++;
		return isl_tab_push_var(tab, isl_tab_undo_redundant, var);
	} else {
		if (row != tab->n_row - 1)
			swap_rows(tab, row, tab->n_row - 1);
		isl_tab_var_from_row(tab, tab->n_row - 1)->index = -1;
		tab->n_row--;
		return 1;
	}
}

int isl_tab_mark_empty(struct isl_tab *tab)
{
	if (!tab)
		return -1;
	if (!tab->empty && tab->need_undo)
		if (isl_tab_push(tab, isl_tab_undo_empty) < 0)
			return -1;
	tab->empty = 1;
	return 0;
}

int isl_tab_freeze_constraint(struct isl_tab *tab, int con)
{
	struct isl_tab_var *var;

	if (!tab)
		return -1;

	var = &tab->con[con];
	if (var->frozen)
		return 0;
	if (var->index < 0)
		return 0;
	var->frozen = 1;

	if (tab->need_undo)
		return isl_tab_push_var(tab, isl_tab_undo_freeze, var);

	return 0;
}

/* Update the rows signs after a pivot of "row" and "col", with "row_sgn"
 * the original sign of the pivot element.
 * We only keep track of row signs during PILP solving and in this case
 * we only pivot a row with negative sign (meaning the value is always
 * non-positive) using a positive pivot element.
 *
 * For each row j, the new value of the parametric constant is equal to
 *
 *	a_j0 - a_jc a_r0/a_rc
 *
 * where a_j0 is the original parametric constant, a_rc is the pivot element,
 * a_r0 is the parametric constant of the pivot row and a_jc is the
 * pivot column entry of the row j.
 * Since a_r0 is non-positive and a_rc is positive, the sign of row j
 * remains the same if a_jc has the same sign as the row j or if
 * a_jc is zero.  In all other cases, we reset the sign to "unknown".
 */
static void update_row_sign(struct isl_tab *tab, int row, int col, int row_sgn)
{
	int i;
	struct isl_mat *mat = tab->mat;
	unsigned off = 2 + tab->M;

	if (!tab->row_sign)
		return;

	if (tab->row_sign[row] == 0)
		return;
	isl_assert(mat->ctx, row_sgn > 0, return);
	isl_assert(mat->ctx, tab->row_sign[row] == isl_tab_row_neg, return);
	tab->row_sign[row] = isl_tab_row_pos;
	for (i = 0; i < tab->n_row; ++i) {
		int s;
		if (i == row)
			continue;
		s = isl_int_sgn(mat->row[i][off + col]);
		if (!s)
			continue;
		if (!tab->row_sign[i])
			continue;
		if (s < 0 && tab->row_sign[i] == isl_tab_row_neg)
			continue;
		if (s > 0 && tab->row_sign[i] == isl_tab_row_pos)
			continue;
		tab->row_sign[i] = isl_tab_row_unknown;
	}
}

/* Given a row number "row" and a column number "col", pivot the tableau
 * such that the associated variables are interchanged.
 * The given row in the tableau expresses
 *
 *	x_r = a_r0 + \sum_i a_ri x_i
 *
 * or
 *
 *	x_c = 1/a_rc x_r - a_r0/a_rc + sum_{i \ne r} -a_ri/a_rc
 *
 * Substituting this equality into the other rows
 *
 *	x_j = a_j0 + \sum_i a_ji x_i
 *
 * with a_jc \ne 0, we obtain
 *
 *	x_j = a_jc/a_rc x_r + a_j0 - a_jc a_r0/a_rc + sum a_ji - a_jc a_ri/a_rc 
 *
 * The tableau
 *
 *	n_rc/d_r		n_ri/d_r
 *	n_jc/d_j		n_ji/d_j
 *
 * where i is any other column and j is any other row,
 * is therefore transformed into
 *
 * s(n_rc)d_r/|n_rc|		-s(n_rc)n_ri/|n_rc|
 * s(n_rc)d_r n_jc/(|n_rc| d_j)	(n_ji |n_rc| - s(n_rc)n_jc n_ri)/(|n_rc| d_j)
 *
 * The transformation is performed along the following steps
 *
 *	d_r/n_rc		n_ri/n_rc
 *	n_jc/d_j		n_ji/d_j
 *
 *	s(n_rc)d_r/|n_rc|	-s(n_rc)n_ri/|n_rc|
 *	n_jc/d_j		n_ji/d_j
 *
 *	s(n_rc)d_r/|n_rc|	-s(n_rc)n_ri/|n_rc|
 *	n_jc/(|n_rc| d_j)	n_ji/(|n_rc| d_j)
 *
 *	s(n_rc)d_r/|n_rc|	-s(n_rc)n_ri/|n_rc|
 *	n_jc/(|n_rc| d_j)	(n_ji |n_rc|)/(|n_rc| d_j)
 *
 *	s(n_rc)d_r/|n_rc|	-s(n_rc)n_ri/|n_rc|
 *	n_jc/(|n_rc| d_j)	(n_ji |n_rc| - s(n_rc)n_jc n_ri)/(|n_rc| d_j)
 *
 * s(n_rc)d_r/|n_rc|		-s(n_rc)n_ri/|n_rc|
 * s(n_rc)d_r n_jc/(|n_rc| d_j)	(n_ji |n_rc| - s(n_rc)n_jc n_ri)/(|n_rc| d_j)
 *
 */
int isl_tab_pivot(struct isl_tab *tab, int row, int col)
{
	int i, j;
	int sgn;
	int t;
	struct isl_mat *mat = tab->mat;
	struct isl_tab_var *var;
	unsigned off = 2 + tab->M;

	isl_int_swap(mat->row[row][0], mat->row[row][off + col]);
	sgn = isl_int_sgn(mat->row[row][0]);
	if (sgn < 0) {
		isl_int_neg(mat->row[row][0], mat->row[row][0]);
		isl_int_neg(mat->row[row][off + col], mat->row[row][off + col]);
	} else
		for (j = 0; j < off - 1 + tab->n_col; ++j) {
			if (j == off - 1 + col)
				continue;
			isl_int_neg(mat->row[row][1 + j], mat->row[row][1 + j]);
		}
	if (!isl_int_is_one(mat->row[row][0]))
		isl_seq_normalize(mat->ctx, mat->row[row], off + tab->n_col);
	for (i = 0; i < tab->n_row; ++i) {
		if (i == row)
			continue;
		if (isl_int_is_zero(mat->row[i][off + col]))
			continue;
		isl_int_mul(mat->row[i][0], mat->row[i][0], mat->row[row][0]);
		for (j = 0; j < off - 1 + tab->n_col; ++j) {
			if (j == off - 1 + col)
				continue;
			isl_int_mul(mat->row[i][1 + j],
				    mat->row[i][1 + j], mat->row[row][0]);
			isl_int_addmul(mat->row[i][1 + j],
				    mat->row[i][off + col], mat->row[row][1 + j]);
		}
		isl_int_mul(mat->row[i][off + col],
			    mat->row[i][off + col], mat->row[row][off + col]);
		if (!isl_int_is_one(mat->row[i][0]))
			isl_seq_normalize(mat->ctx, mat->row[i], off + tab->n_col);
	}
	t = tab->row_var[row];
	tab->row_var[row] = tab->col_var[col];
	tab->col_var[col] = t;
	var = isl_tab_var_from_row(tab, row);
	var->is_row = 1;
	var->index = row;
	var = var_from_col(tab, col);
	var->is_row = 0;
	var->index = col;
	update_row_sign(tab, row, col, sgn);
	if (tab->in_undo)
		return 0;
	for (i = tab->n_redundant; i < tab->n_row; ++i) {
		if (isl_int_is_zero(mat->row[i][off + col]))
			continue;
		if (!isl_tab_var_from_row(tab, i)->frozen &&
		    isl_tab_row_is_redundant(tab, i)) {
			int redo = isl_tab_mark_redundant(tab, i);
			if (redo < 0)
				return -1;
			if (redo)
				--i;
		}
	}
	return 0;
}

/* If "var" represents a column variable, then pivot is up (sgn > 0)
 * or down (sgn < 0) to a row.  The variable is assumed not to be
 * unbounded in the specified direction.
 * If sgn = 0, then the variable is unbounded in both directions,
 * and we pivot with any row we can find.
 */
static int to_row(struct isl_tab *tab, struct isl_tab_var *var, int sign) WARN_UNUSED;
static int to_row(struct isl_tab *tab, struct isl_tab_var *var, int sign)
{
	int r;
	unsigned off = 2 + tab->M;

	if (var->is_row)
		return 0;

	if (sign == 0) {
		for (r = tab->n_redundant; r < tab->n_row; ++r)
			if (!isl_int_is_zero(tab->mat->row[r][off+var->index]))
				break;
		isl_assert(tab->mat->ctx, r < tab->n_row, return -1);
	} else {
		r = pivot_row(tab, NULL, sign, var->index);
		isl_assert(tab->mat->ctx, r >= 0, return -1);
	}

	return isl_tab_pivot(tab, r, var->index);
}

static void check_table(struct isl_tab *tab)
{
	int i;

	if (tab->empty)
		return;
	for (i = 0; i < tab->n_row; ++i) {
		if (!isl_tab_var_from_row(tab, i)->is_nonneg)
			continue;
		assert(!isl_int_is_neg(tab->mat->row[i][1]));
	}
}

/* Return the sign of the maximal value of "var".
 * If the sign is not negative, then on return from this function,
 * the sample value will also be non-negative.
 *
 * If "var" is manifestly unbounded wrt positive values, we are done.
 * Otherwise, we pivot the variable up to a row if needed
 * Then we continue pivoting down until either
 *	- no more down pivots can be performed
 *	- the sample value is positive
 *	- the variable is pivoted into a manifestly unbounded column
 */
static int sign_of_max(struct isl_tab *tab, struct isl_tab_var *var)
{
	int row, col;

	if (max_is_manifestly_unbounded(tab, var))
		return 1;
	if (to_row(tab, var, 1) < 0)
		return -2;
	while (!isl_int_is_pos(tab->mat->row[var->index][1])) {
		find_pivot(tab, var, var, 1, &row, &col);
		if (row == -1)
			return isl_int_sgn(tab->mat->row[var->index][1]);
		if (isl_tab_pivot(tab, row, col) < 0)
			return -2;
		if (!var->is_row) /* manifestly unbounded */
			return 1;
	}
	return 1;
}

static int row_is_neg(struct isl_tab *tab, int row)
{
	if (!tab->M)
		return isl_int_is_neg(tab->mat->row[row][1]);
	if (isl_int_is_pos(tab->mat->row[row][2]))
		return 0;
	if (isl_int_is_neg(tab->mat->row[row][2]))
		return 1;
	return isl_int_is_neg(tab->mat->row[row][1]);
}

static int row_sgn(struct isl_tab *tab, int row)
{
	if (!tab->M)
		return isl_int_sgn(tab->mat->row[row][1]);
	if (!isl_int_is_zero(tab->mat->row[row][2]))
		return isl_int_sgn(tab->mat->row[row][2]);
	else
		return isl_int_sgn(tab->mat->row[row][1]);
}

/* Perform pivots until the row variable "var" has a non-negative
 * sample value or until no more upward pivots can be performed.
 * Return the sign of the sample value after the pivots have been
 * performed.
 */
static int restore_row(struct isl_tab *tab, struct isl_tab_var *var)
{
	int row, col;

	while (row_is_neg(tab, var->index)) {
		find_pivot(tab, var, var, 1, &row, &col);
		if (row == -1)
			break;
		if (isl_tab_pivot(tab, row, col) < 0)
			return -2;
		if (!var->is_row) /* manifestly unbounded */
			return 1;
	}
	return row_sgn(tab, var->index);
}

/* Perform pivots until we are sure that the row variable "var"
 * can attain non-negative values.  After return from this
 * function, "var" is still a row variable, but its sample
 * value may not be non-negative, even if the function returns 1.
 */
static int at_least_zero(struct isl_tab *tab, struct isl_tab_var *var)
{
	int row, col;

	while (isl_int_is_neg(tab->mat->row[var->index][1])) {
		find_pivot(tab, var, var, 1, &row, &col);
		if (row == -1)
			break;
		if (row == var->index) /* manifestly unbounded */
			return 1;
		if (isl_tab_pivot(tab, row, col) < 0)
			return -1;
	}
	return !isl_int_is_neg(tab->mat->row[var->index][1]);
}

/* Return a negative value if "var" can attain negative values.
 * Return a non-negative value otherwise.
 *
 * If "var" is manifestly unbounded wrt negative values, we are done.
 * Otherwise, if var is in a column, we can pivot it down to a row.
 * Then we continue pivoting down until either
 *	- the pivot would result in a manifestly unbounded column
 *	  => we don't perform the pivot, but simply return -1
 *	- no more down pivots can be performed
 *	- the sample value is negative
 * If the sample value becomes negative and the variable is supposed
 * to be nonnegative, then we undo the last pivot.
 * However, if the last pivot has made the pivoting variable
 * obviously redundant, then it may have moved to another row.
 * In that case we look for upward pivots until we reach a non-negative
 * value again.
 */
static int sign_of_min(struct isl_tab *tab, struct isl_tab_var *var)
{
	int row, col;
	struct isl_tab_var *pivot_var = NULL;

	if (min_is_manifestly_unbounded(tab, var))
		return -1;
	if (!var->is_row) {
		col = var->index;
		row = pivot_row(tab, NULL, -1, col);
		pivot_var = var_from_col(tab, col);
		if (isl_tab_pivot(tab, row, col) < 0)
			return -2;
		if (var->is_redundant)
			return 0;
		if (isl_int_is_neg(tab->mat->row[var->index][1])) {
			if (var->is_nonneg) {
				if (!pivot_var->is_redundant &&
				    pivot_var->index == row) {
					if (isl_tab_pivot(tab, row, col) < 0)
						return -2;
				} else
					if (restore_row(tab, var) < -1)
						return -2;
			}
			return -1;
		}
	}
	if (var->is_redundant)
		return 0;
	while (!isl_int_is_neg(tab->mat->row[var->index][1])) {
		find_pivot(tab, var, var, -1, &row, &col);
		if (row == var->index)
			return -1;
		if (row == -1)
			return isl_int_sgn(tab->mat->row[var->index][1]);
		pivot_var = var_from_col(tab, col);
		if (isl_tab_pivot(tab, row, col) < 0)
			return -2;
		if (var->is_redundant)
			return 0;
	}
	if (pivot_var && var->is_nonneg) {
		/* pivot back to non-negative value */
		if (!pivot_var->is_redundant && pivot_var->index == row) {
			if (isl_tab_pivot(tab, row, col) < 0)
				return -2;
		} else
			if (restore_row(tab, var) < -1)
				return -2;
	}
	return -1;
}

static int row_at_most_neg_one(struct isl_tab *tab, int row)
{
	if (tab->M) {
		if (isl_int_is_pos(tab->mat->row[row][2]))
			return 0;
		if (isl_int_is_neg(tab->mat->row[row][2]))
			return 1;
	}
	return isl_int_is_neg(tab->mat->row[row][1]) &&
	       isl_int_abs_ge(tab->mat->row[row][1],
			      tab->mat->row[row][0]);
}

/* Return 1 if "var" can attain values <= -1.
 * Return 0 otherwise.
 *
 * The sample value of "var" is assumed to be non-negative when the
 * the function is called and will be made non-negative again before
 * the function returns.
 */
int isl_tab_min_at_most_neg_one(struct isl_tab *tab, struct isl_tab_var *var)
{
	int row, col;
	struct isl_tab_var *pivot_var;

	if (min_is_manifestly_unbounded(tab, var))
		return 1;
	if (!var->is_row) {
		col = var->index;
		row = pivot_row(tab, NULL, -1, col);
		pivot_var = var_from_col(tab, col);
		if (isl_tab_pivot(tab, row, col) < 0)
			return -1;
		if (var->is_redundant)
			return 0;
		if (row_at_most_neg_one(tab, var->index)) {
			if (var->is_nonneg) {
				if (!pivot_var->is_redundant &&
				    pivot_var->index == row) {
					if (isl_tab_pivot(tab, row, col) < 0)
						return -1;
				} else
					if (restore_row(tab, var) < -1)
						return -1;
			}
			return 1;
		}
	}
	if (var->is_redundant)
		return 0;
	do {
		find_pivot(tab, var, var, -1, &row, &col);
		if (row == var->index)
			return 1;
		if (row == -1)
			return 0;
		pivot_var = var_from_col(tab, col);
		if (isl_tab_pivot(tab, row, col) < 0)
			return -1;
		if (var->is_redundant)
			return 0;
	} while (!row_at_most_neg_one(tab, var->index));
	if (var->is_nonneg) {
		/* pivot back to non-negative value */
		if (!pivot_var->is_redundant && pivot_var->index == row)
			if (isl_tab_pivot(tab, row, col) < 0)
				return -1;
		if (restore_row(tab, var) < -1)
			return -1;
	}
	return 1;
}

/* Return 1 if "var" can attain values >= 1.
 * Return 0 otherwise.
 */
static int at_least_one(struct isl_tab *tab, struct isl_tab_var *var)
{
	int row, col;
	isl_int *r;

	if (max_is_manifestly_unbounded(tab, var))
		return 1;
	if (to_row(tab, var, 1) < 0)
		return -1;
	r = tab->mat->row[var->index];
	while (isl_int_lt(r[1], r[0])) {
		find_pivot(tab, var, var, 1, &row, &col);
		if (row == -1)
			return isl_int_ge(r[1], r[0]);
		if (row == var->index) /* manifestly unbounded */
			return 1;
		if (isl_tab_pivot(tab, row, col) < 0)
			return -1;
	}
	return 1;
}

static void swap_cols(struct isl_tab *tab, int col1, int col2)
{
	int t;
	unsigned off = 2 + tab->M;
	t = tab->col_var[col1];
	tab->col_var[col1] = tab->col_var[col2];
	tab->col_var[col2] = t;
	var_from_col(tab, col1)->index = col1;
	var_from_col(tab, col2)->index = col2;
	tab->mat = isl_mat_swap_cols(tab->mat, off + col1, off + col2);
}

/* Mark column with index "col" as representing a zero variable.
 * If we may need to undo the operation the column is kept,
 * but no longer considered.
 * Otherwise, the column is simply removed.
 *
 * The column may be interchanged with some other column.  If it
 * is interchanged with a later column, return 1.  Otherwise return 0.
 * If the columns are checked in order in the calling function,
 * then a return value of 1 means that the column with the given
 * column number may now contain a different column that
 * hasn't been checked yet.
 */
int isl_tab_kill_col(struct isl_tab *tab, int col)
{
	var_from_col(tab, col)->is_zero = 1;
	if (tab->need_undo) {
		if (isl_tab_push_var(tab, isl_tab_undo_zero,
					    var_from_col(tab, col)) < 0)
			return -1;
		if (col != tab->n_dead)
			swap_cols(tab, col, tab->n_dead);
		tab->n_dead++;
		return 0;
	} else {
		if (col != tab->n_col - 1)
			swap_cols(tab, col, tab->n_col - 1);
		var_from_col(tab, tab->n_col - 1)->index = -1;
		tab->n_col--;
		return 1;
	}
}

/* Row variable "var" is non-negative and cannot attain any values
 * larger than zero.  This means that the coefficients of the unrestricted
 * column variables are zero and that the coefficients of the non-negative
 * column variables are zero or negative.
 * Each of the non-negative variables with a negative coefficient can
 * then also be written as the negative sum of non-negative variables
 * and must therefore also be zero.
 */
static int close_row(struct isl_tab *tab, struct isl_tab_var *var) WARN_UNUSED;
static int close_row(struct isl_tab *tab, struct isl_tab_var *var)
{
	int j;
	struct isl_mat *mat = tab->mat;
	unsigned off = 2 + tab->M;

	isl_assert(tab->mat->ctx, var->is_nonneg, return -1);
	var->is_zero = 1;
	if (tab->need_undo)
		if (isl_tab_push_var(tab, isl_tab_undo_zero, var) < 0)
			return -1;
	for (j = tab->n_dead; j < tab->n_col; ++j) {
		if (isl_int_is_zero(mat->row[var->index][off + j]))
			continue;
		isl_assert(tab->mat->ctx,
		    isl_int_is_neg(mat->row[var->index][off + j]), return -1);
		if (isl_tab_kill_col(tab, j))
			--j;
	}
	if (isl_tab_mark_redundant(tab, var->index) < 0)
		return -1;
	return 0;
}

/* Add a constraint to the tableau and allocate a row for it.
 * Return the index into the constraint array "con".
 */
int isl_tab_allocate_con(struct isl_tab *tab)
{
	int r;

	isl_assert(tab->mat->ctx, tab->n_row < tab->mat->n_row, return -1);
	isl_assert(tab->mat->ctx, tab->n_con < tab->max_con, return -1);

	r = tab->n_con;
	tab->con[r].index = tab->n_row;
	tab->con[r].is_row = 1;
	tab->con[r].is_nonneg = 0;
	tab->con[r].is_zero = 0;
	tab->con[r].is_redundant = 0;
	tab->con[r].frozen = 0;
	tab->con[r].negated = 0;
	tab->row_var[tab->n_row] = ~r;

	tab->n_row++;
	tab->n_con++;
	if (isl_tab_push_var(tab, isl_tab_undo_allocate, &tab->con[r]) < 0)
		return -1;

	return r;
}

/* Add a variable to the tableau and allocate a column for it.
 * Return the index into the variable array "var".
 */
int isl_tab_allocate_var(struct isl_tab *tab)
{
	int r;
	int i;
	unsigned off = 2 + tab->M;

	isl_assert(tab->mat->ctx, tab->n_col < tab->mat->n_col, return -1);
	isl_assert(tab->mat->ctx, tab->n_var < tab->max_var, return -1);

	r = tab->n_var;
	tab->var[r].index = tab->n_col;
	tab->var[r].is_row = 0;
	tab->var[r].is_nonneg = 0;
	tab->var[r].is_zero = 0;
	tab->var[r].is_redundant = 0;
	tab->var[r].frozen = 0;
	tab->var[r].negated = 0;
	tab->col_var[tab->n_col] = r;

	for (i = 0; i < tab->n_row; ++i)
		isl_int_set_si(tab->mat->row[i][off + tab->n_col], 0);

	tab->n_var++;
	tab->n_col++;
	if (isl_tab_push_var(tab, isl_tab_undo_allocate, &tab->var[r]) < 0)
		return -1;

	return r;
}

/* Add a row to the tableau.  The row is given as an affine combination
 * of the original variables and needs to be expressed in terms of the
 * column variables.
 *
 * We add each term in turn.
 * If r = n/d_r is the current sum and we need to add k x, then
 * 	if x is a column variable, we increase the numerator of
 *		this column by k d_r
 *	if x = f/d_x is a row variable, then the new representation of r is
 *
 *		 n    k f   d_x/g n + d_r/g k f   m/d_r n + m/d_g k f
 *		--- + --- = ------------------- = -------------------
 *		d_r   d_r        d_r d_x/g                m
 *
 *	with g the gcd of d_r and d_x and m the lcm of d_r and d_x.
 */
int isl_tab_add_row(struct isl_tab *tab, isl_int *line)
{
	int i;
	int r;
	isl_int *row;
	isl_int a, b;
	unsigned off = 2 + tab->M;

	r = isl_tab_allocate_con(tab);
	if (r < 0)
		return -1;

	isl_int_init(a);
	isl_int_init(b);
	row = tab->mat->row[tab->con[r].index];
	isl_int_set_si(row[0], 1);
	isl_int_set(row[1], line[0]);
	isl_seq_clr(row + 2, tab->M + tab->n_col);
	for (i = 0; i < tab->n_var; ++i) {
		if (tab->var[i].is_zero)
			continue;
		if (tab->var[i].is_row) {
			isl_int_lcm(a,
				row[0], tab->mat->row[tab->var[i].index][0]);
			isl_int_swap(a, row[0]);
			isl_int_divexact(a, row[0], a);
			isl_int_divexact(b,
				row[0], tab->mat->row[tab->var[i].index][0]);
			isl_int_mul(b, b, line[1 + i]);
			isl_seq_combine(row + 1, a, row + 1,
			    b, tab->mat->row[tab->var[i].index] + 1,
			    1 + tab->M + tab->n_col);
		} else
			isl_int_addmul(row[off + tab->var[i].index],
							line[1 + i], row[0]);
		if (tab->M && i >= tab->n_param && i < tab->n_var - tab->n_div)
			isl_int_submul(row[2], line[1 + i], row[0]);
	}
	isl_seq_normalize(tab->mat->ctx, row, off + tab->n_col);
	isl_int_clear(a);
	isl_int_clear(b);

	if (tab->row_sign)
		tab->row_sign[tab->con[r].index] = 0;

	return r;
}

static int drop_row(struct isl_tab *tab, int row)
{
	isl_assert(tab->mat->ctx, ~tab->row_var[row] == tab->n_con - 1, return -1);
	if (row != tab->n_row - 1)
		swap_rows(tab, row, tab->n_row - 1);
	tab->n_row--;
	tab->n_con--;
	return 0;
}

static int drop_col(struct isl_tab *tab, int col)
{
	isl_assert(tab->mat->ctx, tab->col_var[col] == tab->n_var - 1, return -1);
	if (col != tab->n_col - 1)
		swap_cols(tab, col, tab->n_col - 1);
	tab->n_col--;
	tab->n_var--;
	return 0;
}

/* Add inequality "ineq" and check if it conflicts with the
 * previously added constraints or if it is obviously redundant.
 */
int isl_tab_add_ineq(struct isl_tab *tab, isl_int *ineq)
{
	int r;
	int sgn;
	isl_int cst;

	if (!tab)
		return -1;
	if (tab->bset) {
		struct isl_basic_set *bset = tab->bset;

		isl_assert(tab->mat->ctx, tab->n_eq == bset->n_eq, return -1);
		isl_assert(tab->mat->ctx,
			    tab->n_con == bset->n_eq + bset->n_ineq, return -1);
		tab->bset = isl_basic_set_add_ineq(tab->bset, ineq);
		if (isl_tab_push(tab, isl_tab_undo_bset_ineq) < 0)
			return -1;
		if (!tab->bset)
			return -1;
	}
	if (tab->cone) {
		isl_int_init(cst);
		isl_int_swap(ineq[0], cst);
	}
	r = isl_tab_add_row(tab, ineq);
	if (tab->cone) {
		isl_int_swap(ineq[0], cst);
		isl_int_clear(cst);
	}
	if (r < 0)
		return -1;
	tab->con[r].is_nonneg = 1;
	if (isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r]) < 0)
		return -1;
	if (isl_tab_row_is_redundant(tab, tab->con[r].index)) {
		if (isl_tab_mark_redundant(tab, tab->con[r].index) < 0)
			return -1;
		return 0;
	}

	sgn = restore_row(tab, &tab->con[r]);
	if (sgn < -1)
		return -1;
	if (sgn < 0)
		return isl_tab_mark_empty(tab);
	if (tab->con[r].is_row && isl_tab_row_is_redundant(tab, tab->con[r].index))
		if (isl_tab_mark_redundant(tab, tab->con[r].index) < 0)
			return -1;
	return 0;
}

/* Pivot a non-negative variable down until it reaches the value zero
 * and then pivot the variable into a column position.
 */
static int to_col(struct isl_tab *tab, struct isl_tab_var *var) WARN_UNUSED;
static int to_col(struct isl_tab *tab, struct isl_tab_var *var)
{
	int i;
	int row, col;
	unsigned off = 2 + tab->M;

	if (!var->is_row)
		return 0;

	while (isl_int_is_pos(tab->mat->row[var->index][1])) {
		find_pivot(tab, var, NULL, -1, &row, &col);
		isl_assert(tab->mat->ctx, row != -1, return -1);
		if (isl_tab_pivot(tab, row, col) < 0)
			return -1;
		if (!var->is_row)
			return 0;
	}

	for (i = tab->n_dead; i < tab->n_col; ++i)
		if (!isl_int_is_zero(tab->mat->row[var->index][off + i]))
			break;

	isl_assert(tab->mat->ctx, i < tab->n_col, return -1);
	if (isl_tab_pivot(tab, var->index, i) < 0)
		return -1;

	return 0;
}

/* We assume Gaussian elimination has been performed on the equalities.
 * The equalities can therefore never conflict.
 * Adding the equalities is currently only really useful for a later call
 * to isl_tab_ineq_type.
 */
static struct isl_tab *add_eq(struct isl_tab *tab, isl_int *eq)
{
	int i;
	int r;

	if (!tab)
		return NULL;
	r = isl_tab_add_row(tab, eq);
	if (r < 0)
		goto error;

	r = tab->con[r].index;
	i = isl_seq_first_non_zero(tab->mat->row[r] + 2 + tab->M + tab->n_dead,
					tab->n_col - tab->n_dead);
	isl_assert(tab->mat->ctx, i >= 0, goto error);
	i += tab->n_dead;
	if (isl_tab_pivot(tab, r, i) < 0)
		goto error;
	if (isl_tab_kill_col(tab, i) < 0)
		goto error;
	tab->n_eq++;

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

static int row_is_manifestly_zero(struct isl_tab *tab, int row)
{
	unsigned off = 2 + tab->M;

	if (!isl_int_is_zero(tab->mat->row[row][1]))
		return 0;
	if (tab->M && !isl_int_is_zero(tab->mat->row[row][2]))
		return 0;
	return isl_seq_first_non_zero(tab->mat->row[row] + off + tab->n_dead,
					tab->n_col - tab->n_dead) == -1;
}

/* Add an equality that is known to be valid for the given tableau.
 */
struct isl_tab *isl_tab_add_valid_eq(struct isl_tab *tab, isl_int *eq)
{
	struct isl_tab_var *var;
	int r;

	if (!tab)
		return NULL;
	r = isl_tab_add_row(tab, eq);
	if (r < 0)
		goto error;

	var = &tab->con[r];
	r = var->index;
	if (row_is_manifestly_zero(tab, r)) {
		var->is_zero = 1;
		if (isl_tab_mark_redundant(tab, r) < 0)
			goto error;
		return tab;
	}

	if (isl_int_is_neg(tab->mat->row[r][1])) {
		isl_seq_neg(tab->mat->row[r] + 1, tab->mat->row[r] + 1,
			    1 + tab->n_col);
		var->negated = 1;
	}
	var->is_nonneg = 1;
	if (to_col(tab, var) < 0)
		goto error;
	var->is_nonneg = 0;
	if (isl_tab_kill_col(tab, var->index) < 0)
		goto error;

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

static int add_zero_row(struct isl_tab *tab)
{
	int r;
	isl_int *row;

	r = isl_tab_allocate_con(tab);
	if (r < 0)
		return -1;

	row = tab->mat->row[tab->con[r].index];
	isl_seq_clr(row + 1, 1 + tab->M + tab->n_col);
	isl_int_set_si(row[0], 1);

	return r;
}

/* Add equality "eq" and check if it conflicts with the
 * previously added constraints or if it is obviously redundant.
 */
struct isl_tab *isl_tab_add_eq(struct isl_tab *tab, isl_int *eq)
{
	struct isl_tab_undo *snap = NULL;
	struct isl_tab_var *var;
	int r;
	int row;
	int sgn;
	isl_int cst;

	if (!tab)
		return NULL;
	isl_assert(tab->mat->ctx, !tab->M, goto error);

	if (tab->need_undo)
		snap = isl_tab_snap(tab);

	if (tab->cone) {
		isl_int_init(cst);
		isl_int_swap(eq[0], cst);
	}
	r = isl_tab_add_row(tab, eq);
	if (tab->cone) {
		isl_int_swap(eq[0], cst);
		isl_int_clear(cst);
	}
	if (r < 0)
		goto error;

	var = &tab->con[r];
	row = var->index;
	if (row_is_manifestly_zero(tab, row)) {
		if (snap) {
			if (isl_tab_rollback(tab, snap) < 0)
				goto error;
		} else
			drop_row(tab, row);
		return tab;
	}

	if (tab->bset) {
		tab->bset = isl_basic_set_add_ineq(tab->bset, eq);
		if (isl_tab_push(tab, isl_tab_undo_bset_ineq) < 0)
			goto error;
		isl_seq_neg(eq, eq, 1 + tab->n_var);
		tab->bset = isl_basic_set_add_ineq(tab->bset, eq);
		isl_seq_neg(eq, eq, 1 + tab->n_var);
		if (isl_tab_push(tab, isl_tab_undo_bset_ineq) < 0)
			goto error;
		if (!tab->bset)
			goto error;
		if (add_zero_row(tab) < 0)
			goto error;
	}

	sgn = isl_int_sgn(tab->mat->row[row][1]);

	if (sgn > 0) {
		isl_seq_neg(tab->mat->row[row] + 1, tab->mat->row[row] + 1,
			    1 + tab->n_col);
		var->negated = 1;
		sgn = -1;
	}

	if (sgn < 0) {
		sgn = sign_of_max(tab, var);
		if (sgn < -1)
			goto error;
		if (sgn < 0) {
			if (isl_tab_mark_empty(tab) < 0)
				goto error;
			return tab;
		}
	}

	var->is_nonneg = 1;
	if (to_col(tab, var) < 0)
		goto error;
	var->is_nonneg = 0;
	if (isl_tab_kill_col(tab, var->index) < 0)
		goto error;

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

struct isl_tab *isl_tab_from_basic_map(struct isl_basic_map *bmap)
{
	int i;
	struct isl_tab *tab;

	if (!bmap)
		return NULL;
	tab = isl_tab_alloc(bmap->ctx,
			    isl_basic_map_total_dim(bmap) + bmap->n_ineq + 1,
			    isl_basic_map_total_dim(bmap), 0);
	if (!tab)
		return NULL;
	tab->rational = ISL_F_ISSET(bmap, ISL_BASIC_MAP_RATIONAL);
	if (ISL_F_ISSET(bmap, ISL_BASIC_MAP_EMPTY)) {
		if (isl_tab_mark_empty(tab) < 0)
			goto error;
		return tab;
	}
	for (i = 0; i < bmap->n_eq; ++i) {
		tab = add_eq(tab, bmap->eq[i]);
		if (!tab)
			return tab;
	}
	for (i = 0; i < bmap->n_ineq; ++i) {
		if (isl_tab_add_ineq(tab, bmap->ineq[i]) < 0)
			goto error;
		if (tab->empty)
			return tab;
	}
	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

struct isl_tab *isl_tab_from_basic_set(struct isl_basic_set *bset)
{
	return isl_tab_from_basic_map((struct isl_basic_map *)bset);
}

/* Construct a tableau corresponding to the recession cone of "bset".
 */
struct isl_tab *isl_tab_from_recession_cone(struct isl_basic_set *bset)
{
	isl_int cst;
	int i;
	struct isl_tab *tab;

	if (!bset)
		return NULL;
	tab = isl_tab_alloc(bset->ctx, bset->n_eq + bset->n_ineq,
				isl_basic_set_total_dim(bset), 0);
	if (!tab)
		return NULL;
	tab->rational = ISL_F_ISSET(bset, ISL_BASIC_SET_RATIONAL);
	tab->cone = 1;

	isl_int_init(cst);
	for (i = 0; i < bset->n_eq; ++i) {
		isl_int_swap(bset->eq[i][0], cst);
		tab = add_eq(tab, bset->eq[i]);
		isl_int_swap(bset->eq[i][0], cst);
		if (!tab)
			goto done;
	}
	for (i = 0; i < bset->n_ineq; ++i) {
		int r;
		isl_int_swap(bset->ineq[i][0], cst);
		r = isl_tab_add_row(tab, bset->ineq[i]);
		isl_int_swap(bset->ineq[i][0], cst);
		if (r < 0)
			goto error;
		tab->con[r].is_nonneg = 1;
		if (isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r]) < 0)
			goto error;
	}
done:
	isl_int_clear(cst);
	return tab;
error:
	isl_int_clear(cst);
	isl_tab_free(tab);
	return NULL;
}

/* Assuming "tab" is the tableau of a cone, check if the cone is
 * bounded, i.e., if it is empty or only contains the origin.
 */
int isl_tab_cone_is_bounded(struct isl_tab *tab)
{
	int i;

	if (!tab)
		return -1;
	if (tab->empty)
		return 1;
	if (tab->n_dead == tab->n_col)
		return 1;

	for (;;) {
		for (i = tab->n_redundant; i < tab->n_row; ++i) {
			struct isl_tab_var *var;
			int sgn;
			var = isl_tab_var_from_row(tab, i);
			if (!var->is_nonneg)
				continue;
			sgn = sign_of_max(tab, var);
			if (sgn < -1)
				return -1;
			if (sgn != 0)
				return 0;
			if (close_row(tab, var) < 0)
				return -1;
			break;
		}
		if (tab->n_dead == tab->n_col)
			return 1;
		if (i == tab->n_row)
			return 0;
	}
}

int isl_tab_sample_is_integer(struct isl_tab *tab)
{
	int i;

	if (!tab)
		return -1;

	for (i = 0; i < tab->n_var; ++i) {
		int row;
		if (!tab->var[i].is_row)
			continue;
		row = tab->var[i].index;
		if (!isl_int_is_divisible_by(tab->mat->row[row][1],
						tab->mat->row[row][0]))
			return 0;
	}
	return 1;
}

static struct isl_vec *extract_integer_sample(struct isl_tab *tab)
{
	int i;
	struct isl_vec *vec;

	vec = isl_vec_alloc(tab->mat->ctx, 1 + tab->n_var);
	if (!vec)
		return NULL;

	isl_int_set_si(vec->block.data[0], 1);
	for (i = 0; i < tab->n_var; ++i) {
		if (!tab->var[i].is_row)
			isl_int_set_si(vec->block.data[1 + i], 0);
		else {
			int row = tab->var[i].index;
			isl_int_divexact(vec->block.data[1 + i],
				tab->mat->row[row][1], tab->mat->row[row][0]);
		}
	}

	return vec;
}

struct isl_vec *isl_tab_get_sample_value(struct isl_tab *tab)
{
	int i;
	struct isl_vec *vec;
	isl_int m;

	if (!tab)
		return NULL;

	vec = isl_vec_alloc(tab->mat->ctx, 1 + tab->n_var);
	if (!vec)
		return NULL;

	isl_int_init(m);

	isl_int_set_si(vec->block.data[0], 1);
	for (i = 0; i < tab->n_var; ++i) {
		int row;
		if (!tab->var[i].is_row) {
			isl_int_set_si(vec->block.data[1 + i], 0);
			continue;
		}
		row = tab->var[i].index;
		isl_int_gcd(m, vec->block.data[0], tab->mat->row[row][0]);
		isl_int_divexact(m, tab->mat->row[row][0], m);
		isl_seq_scale(vec->block.data, vec->block.data, m, 1 + i);
		isl_int_divexact(m, vec->block.data[0], tab->mat->row[row][0]);
		isl_int_mul(vec->block.data[1 + i], m, tab->mat->row[row][1]);
	}
	vec = isl_vec_normalize(vec);

	isl_int_clear(m);
	return vec;
}

/* Update "bmap" based on the results of the tableau "tab".
 * In particular, implicit equalities are made explicit, redundant constraints
 * are removed and if the sample value happens to be integer, it is stored
 * in "bmap" (unless "bmap" already had an integer sample).
 *
 * The tableau is assumed to have been created from "bmap" using
 * isl_tab_from_basic_map.
 */
struct isl_basic_map *isl_basic_map_update_from_tab(struct isl_basic_map *bmap,
	struct isl_tab *tab)
{
	int i;
	unsigned n_eq;

	if (!bmap)
		return NULL;
	if (!tab)
		return bmap;

	n_eq = tab->n_eq;
	if (tab->empty)
		bmap = isl_basic_map_set_to_empty(bmap);
	else
		for (i = bmap->n_ineq - 1; i >= 0; --i) {
			if (isl_tab_is_equality(tab, n_eq + i))
				isl_basic_map_inequality_to_equality(bmap, i);
			else if (isl_tab_is_redundant(tab, n_eq + i))
				isl_basic_map_drop_inequality(bmap, i);
		}
	if (!tab->rational &&
	    !bmap->sample && isl_tab_sample_is_integer(tab))
		bmap->sample = extract_integer_sample(tab);
	return bmap;
}

struct isl_basic_set *isl_basic_set_update_from_tab(struct isl_basic_set *bset,
	struct isl_tab *tab)
{
	return (struct isl_basic_set *)isl_basic_map_update_from_tab(
		(struct isl_basic_map *)bset, tab);
}

/* Given a non-negative variable "var", add a new non-negative variable
 * that is the opposite of "var", ensuring that var can only attain the
 * value zero.
 * If var = n/d is a row variable, then the new variable = -n/d.
 * If var is a column variables, then the new variable = -var.
 * If the new variable cannot attain non-negative values, then
 * the resulting tableau is empty.
 * Otherwise, we know the value will be zero and we close the row.
 */
static struct isl_tab *cut_to_hyperplane(struct isl_tab *tab,
	struct isl_tab_var *var)
{
	unsigned r;
	isl_int *row;
	int sgn;
	unsigned off = 2 + tab->M;

	if (var->is_zero)
		return tab;
	isl_assert(tab->mat->ctx, !var->is_redundant, goto error);

	if (isl_tab_extend_cons(tab, 1) < 0)
		goto error;

	r = tab->n_con;
	tab->con[r].index = tab->n_row;
	tab->con[r].is_row = 1;
	tab->con[r].is_nonneg = 0;
	tab->con[r].is_zero = 0;
	tab->con[r].is_redundant = 0;
	tab->con[r].frozen = 0;
	tab->con[r].negated = 0;
	tab->row_var[tab->n_row] = ~r;
	row = tab->mat->row[tab->n_row];

	if (var->is_row) {
		isl_int_set(row[0], tab->mat->row[var->index][0]);
		isl_seq_neg(row + 1,
			    tab->mat->row[var->index] + 1, 1 + tab->n_col);
	} else {
		isl_int_set_si(row[0], 1);
		isl_seq_clr(row + 1, 1 + tab->n_col);
		isl_int_set_si(row[off + var->index], -1);
	}

	tab->n_row++;
	tab->n_con++;
	if (isl_tab_push_var(tab, isl_tab_undo_allocate, &tab->con[r]) < 0)
		goto error;

	sgn = sign_of_max(tab, &tab->con[r]);
	if (sgn < -1)
		goto error;
	if (sgn < 0) {
		if (isl_tab_mark_empty(tab) < 0)
			goto error;
		return tab;
	}
	tab->con[r].is_nonneg = 1;
	if (isl_tab_push_var(tab, isl_tab_undo_nonneg, &tab->con[r]) < 0)
		goto error;
	/* sgn == 0 */
	if (close_row(tab, &tab->con[r]) < 0)
		goto error;

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

/* Given a tableau "tab" and an inequality constraint "con" of the tableau,
 * relax the inequality by one.  That is, the inequality r >= 0 is replaced
 * by r' = r + 1 >= 0.
 * If r is a row variable, we simply increase the constant term by one
 * (taking into account the denominator).
 * If r is a column variable, then we need to modify each row that
 * refers to r = r' - 1 by substituting this equality, effectively
 * subtracting the coefficient of the column from the constant.
 */
struct isl_tab *isl_tab_relax(struct isl_tab *tab, int con)
{
	struct isl_tab_var *var;
	unsigned off = 2 + tab->M;

	if (!tab)
		return NULL;

	var = &tab->con[con];

	if (!var->is_row && !max_is_manifestly_unbounded(tab, var))
		if (to_row(tab, var, 1) < 0)
			goto error;

	if (var->is_row)
		isl_int_add(tab->mat->row[var->index][1],
		    tab->mat->row[var->index][1], tab->mat->row[var->index][0]);
	else {
		int i;

		for (i = 0; i < tab->n_row; ++i) {
			if (isl_int_is_zero(tab->mat->row[i][off + var->index]))
				continue;
			isl_int_sub(tab->mat->row[i][1], tab->mat->row[i][1],
			    tab->mat->row[i][off + var->index]);
		}

	}

	if (isl_tab_push_var(tab, isl_tab_undo_relax, var) < 0)
		goto error;

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

struct isl_tab *isl_tab_select_facet(struct isl_tab *tab, int con)
{
	if (!tab)
		return NULL;

	return cut_to_hyperplane(tab, &tab->con[con]);
}

static int may_be_equality(struct isl_tab *tab, int row)
{
	unsigned off = 2 + tab->M;
	return (tab->rational ? isl_int_is_zero(tab->mat->row[row][1])
			      : isl_int_lt(tab->mat->row[row][1],
					    tab->mat->row[row][0])) &&
		isl_seq_first_non_zero(tab->mat->row[row] + off + tab->n_dead,
					tab->n_col - tab->n_dead) != -1;
}

/* Check for (near) equalities among the constraints.
 * A constraint is an equality if it is non-negative and if
 * its maximal value is either
 *	- zero (in case of rational tableaus), or
 *	- strictly less than 1 (in case of integer tableaus)
 *
 * We first mark all non-redundant and non-dead variables that
 * are not frozen and not obviously not an equality.
 * Then we iterate over all marked variables if they can attain
 * any values larger than zero or at least one.
 * If the maximal value is zero, we mark any column variables
 * that appear in the row as being zero and mark the row as being redundant.
 * Otherwise, if the maximal value is strictly less than one (and the
 * tableau is integer), then we restrict the value to being zero
 * by adding an opposite non-negative variable.
 */
struct isl_tab *isl_tab_detect_implicit_equalities(struct isl_tab *tab)
{
	int i;
	unsigned n_marked;

	if (!tab)
		return NULL;
	if (tab->empty)
		return tab;
	if (tab->n_dead == tab->n_col)
		return tab;

	n_marked = 0;
	for (i = tab->n_redundant; i < tab->n_row; ++i) {
		struct isl_tab_var *var = isl_tab_var_from_row(tab, i);
		var->marked = !var->frozen && var->is_nonneg &&
			may_be_equality(tab, i);
		if (var->marked)
			n_marked++;
	}
	for (i = tab->n_dead; i < tab->n_col; ++i) {
		struct isl_tab_var *var = var_from_col(tab, i);
		var->marked = !var->frozen && var->is_nonneg;
		if (var->marked)
			n_marked++;
	}
	while (n_marked) {
		struct isl_tab_var *var;
		int sgn;
		for (i = tab->n_redundant; i < tab->n_row; ++i) {
			var = isl_tab_var_from_row(tab, i);
			if (var->marked)
				break;
		}
		if (i == tab->n_row) {
			for (i = tab->n_dead; i < tab->n_col; ++i) {
				var = var_from_col(tab, i);
				if (var->marked)
					break;
			}
			if (i == tab->n_col)
				break;
		}
		var->marked = 0;
		n_marked--;
		sgn = sign_of_max(tab, var);
		if (sgn < 0)
			goto error;
		if (sgn == 0) {
			if (close_row(tab, var) < 0)
				goto error;
		} else if (!tab->rational && !at_least_one(tab, var)) {
			tab = cut_to_hyperplane(tab, var);
			return isl_tab_detect_implicit_equalities(tab);
		}
		for (i = tab->n_redundant; i < tab->n_row; ++i) {
			var = isl_tab_var_from_row(tab, i);
			if (!var->marked)
				continue;
			if (may_be_equality(tab, i))
				continue;
			var->marked = 0;
			n_marked--;
		}
	}

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

static int con_is_redundant(struct isl_tab *tab, struct isl_tab_var *var)
{
	if (!tab)
		return -1;
	if (tab->rational) {
		int sgn = sign_of_min(tab, var);
		if (sgn < -1)
			return -1;
		return sgn >= 0;
	} else {
		int irred = isl_tab_min_at_most_neg_one(tab, var);
		if (irred < 0)
			return -1;
		return !irred;
	}
}

/* Check for (near) redundant constraints.
 * A constraint is redundant if it is non-negative and if
 * its minimal value (temporarily ignoring the non-negativity) is either
 *	- zero (in case of rational tableaus), or
 *	- strictly larger than -1 (in case of integer tableaus)
 *
 * We first mark all non-redundant and non-dead variables that
 * are not frozen and not obviously negatively unbounded.
 * Then we iterate over all marked variables if they can attain
 * any values smaller than zero or at most negative one.
 * If not, we mark the row as being redundant (assuming it hasn't
 * been detected as being obviously redundant in the mean time).
 */
struct isl_tab *isl_tab_detect_redundant(struct isl_tab *tab)
{
	int i;
	unsigned n_marked;

	if (!tab)
		return NULL;
	if (tab->empty)
		return tab;
	if (tab->n_redundant == tab->n_row)
		return tab;

	n_marked = 0;
	for (i = tab->n_redundant; i < tab->n_row; ++i) {
		struct isl_tab_var *var = isl_tab_var_from_row(tab, i);
		var->marked = !var->frozen && var->is_nonneg;
		if (var->marked)
			n_marked++;
	}
	for (i = tab->n_dead; i < tab->n_col; ++i) {
		struct isl_tab_var *var = var_from_col(tab, i);
		var->marked = !var->frozen && var->is_nonneg &&
			!min_is_manifestly_unbounded(tab, var);
		if (var->marked)
			n_marked++;
	}
	while (n_marked) {
		struct isl_tab_var *var;
		int red;
		for (i = tab->n_redundant; i < tab->n_row; ++i) {
			var = isl_tab_var_from_row(tab, i);
			if (var->marked)
				break;
		}
		if (i == tab->n_row) {
			for (i = tab->n_dead; i < tab->n_col; ++i) {
				var = var_from_col(tab, i);
				if (var->marked)
					break;
			}
			if (i == tab->n_col)
				break;
		}
		var->marked = 0;
		n_marked--;
		red = con_is_redundant(tab, var);
		if (red < 0)
			goto error;
		if (red && !var->is_redundant)
			if (isl_tab_mark_redundant(tab, var->index) < 0)
				goto error;
		for (i = tab->n_dead; i < tab->n_col; ++i) {
			var = var_from_col(tab, i);
			if (!var->marked)
				continue;
			if (!min_is_manifestly_unbounded(tab, var))
				continue;
			var->marked = 0;
			n_marked--;
		}
	}

	return tab;
error:
	isl_tab_free(tab);
	return NULL;
}

int isl_tab_is_equality(struct isl_tab *tab, int con)
{
	int row;
	unsigned off;

	if (!tab)
		return -1;
	if (tab->con[con].is_zero)
		return 1;
	if (tab->con[con].is_redundant)
		return 0;
	if (!tab->con[con].is_row)
		return tab->con[con].index < tab->n_dead;

	row = tab->con[con].index;

	off = 2 + tab->M;
	return isl_int_is_zero(tab->mat->row[row][1]) &&
		isl_seq_first_non_zero(tab->mat->row[row] + 2 + tab->n_dead,
					tab->n_col - tab->n_dead) == -1;
}

/* Return the minimial value of the affine expression "f" with denominator
 * "denom" in *opt, *opt_denom, assuming the tableau is not empty and
 * the expression cannot attain arbitrarily small values.
 * If opt_denom is NULL, then *opt is rounded up to the nearest integer.
 * The return value reflects the nature of the result (empty, unbounded,
 * minmimal value returned in *opt).
 */
enum isl_lp_result isl_tab_min(struct isl_tab *tab,
	isl_int *f, isl_int denom, isl_int *opt, isl_int *opt_denom,
	unsigned flags)
{
	int r;
	enum isl_lp_result res = isl_lp_ok;
	struct isl_tab_var *var;
	struct isl_tab_undo *snap;

	if (tab->empty)
		return isl_lp_empty;

	snap = isl_tab_snap(tab);
	r = isl_tab_add_row(tab, f);
	if (r < 0)
		return isl_lp_error;
	var = &tab->con[r];
	isl_int_mul(tab->mat->row[var->index][0],
		    tab->mat->row[var->index][0], denom);
	for (;;) {
		int row, col;
		find_pivot(tab, var, var, -1, &row, &col);
		if (row == var->index) {
			res = isl_lp_unbounded;
			break;
		}
		if (row == -1)
			break;
		if (isl_tab_pivot(tab, row, col) < 0)
			return isl_lp_error;
	}
	if (ISL_FL_ISSET(flags, ISL_TAB_SAVE_DUAL)) {
		int i;

		isl_vec_free(tab->dual);
		tab->dual = isl_vec_alloc(tab->mat->ctx, 1 + tab->n_con);
		if (!tab->dual)
			return isl_lp_error;
		isl_int_set(tab->dual->el[0], tab->mat->row[var->index][0]);
		for (i = 0; i < tab->n_con; ++i) {
			int pos;
			if (tab->con[i].is_row) {
				isl_int_set_si(tab->dual->el[1 + i], 0);
				continue;
			}
			pos = 2 + tab->M + tab->con[i].index;
			if (tab->con[i].negated)
				isl_int_neg(tab->dual->el[1 + i],
					    tab->mat->row[var->index][pos]);
			else
				isl_int_set(tab->dual->el[1 + i],
					    tab->mat->row[var->index][pos]);
		}
	}
	if (opt && res == isl_lp_ok) {
		if (opt_denom) {
			isl_int_set(*opt, tab->mat->row[var->index][1]);
			isl_int_set(*opt_denom, tab->mat->row[var->index][0]);
		} else
			isl_int_cdiv_q(*opt, tab->mat->row[var->index][1],
					     tab->mat->row[var->index][0]);
	}
	if (isl_tab_rollback(tab, snap) < 0)
		return isl_lp_error;
	return res;
}

int isl_tab_is_redundant(struct isl_tab *tab, int con)
{
	if (!tab)
		return -1;
	if (tab->con[con].is_zero)
		return 0;
	if (tab->con[con].is_redundant)
		return 1;
	return tab->con[con].is_row && tab->con[con].index < tab->n_redundant;
}

/* Take a snapshot of the tableau that can be restored by s call to
 * isl_tab_rollback.
 */
struct isl_tab_undo *isl_tab_snap(struct isl_tab *tab)
{
	if (!tab)
		return NULL;
	tab->need_undo = 1;
	return tab->top;
}

/* Undo the operation performed by isl_tab_relax.
 */
static int unrelax(struct isl_tab *tab, struct isl_tab_var *var) WARN_UNUSED;
static int unrelax(struct isl_tab *tab, struct isl_tab_var *var)
{
	unsigned off = 2 + tab->M;

	if (!var->is_row && !max_is_manifestly_unbounded(tab, var))
		if (to_row(tab, var, 1) < 0)
			return -1;

	if (var->is_row)
		isl_int_sub(tab->mat->row[var->index][1],
		    tab->mat->row[var->index][1], tab->mat->row[var->index][0]);
	else {
		int i;

		for (i = 0; i < tab->n_row; ++i) {
			if (isl_int_is_zero(tab->mat->row[i][off + var->index]))
				continue;
			isl_int_add(tab->mat->row[i][1], tab->mat->row[i][1],
			    tab->mat->row[i][off + var->index]);
		}

	}

	return 0;
}

static int perform_undo_var(struct isl_tab *tab, struct isl_tab_undo *undo) WARN_UNUSED;
static int perform_undo_var(struct isl_tab *tab, struct isl_tab_undo *undo)
{
	struct isl_tab_var *var = var_from_index(tab, undo->u.var_index);
	switch(undo->type) {
	case isl_tab_undo_nonneg:
		var->is_nonneg = 0;
		break;
	case isl_tab_undo_redundant:
		var->is_redundant = 0;
		tab->n_redundant--;
		break;
	case isl_tab_undo_freeze:
		var->frozen = 0;
		break;
	case isl_tab_undo_zero:
		var->is_zero = 0;
		if (!var->is_row)
			tab->n_dead--;
		break;
	case isl_tab_undo_allocate:
		if (undo->u.var_index >= 0) {
			isl_assert(tab->mat->ctx, !var->is_row, return -1);
			drop_col(tab, var->index);
			break;
		}
		if (!var->is_row) {
			if (!max_is_manifestly_unbounded(tab, var)) {
				if (to_row(tab, var, 1) < 0)
					return -1;
			} else if (!min_is_manifestly_unbounded(tab, var)) {
				if (to_row(tab, var, -1) < 0)
					return -1;
			} else
				if (to_row(tab, var, 0) < 0)
					return -1;
		}
		drop_row(tab, var->index);
		break;
	case isl_tab_undo_relax:
		return unrelax(tab, var);
	}

	return 0;
}

/* Restore the tableau to the state where the basic variables
 * are those in "col_var".
 * We first construct a list of variables that are currently in
 * the basis, but shouldn't.  Then we iterate over all variables
 * that should be in the basis and for each one that is currently
 * not in the basis, we exchange it with one of the elements of the
 * list constructed before.
 * We can always find an appropriate variable to pivot with because
 * the current basis is mapped to the old basis by a non-singular
 * matrix and so we can never end up with a zero row.
 */
static int restore_basis(struct isl_tab *tab, int *col_var)
{
	int i, j;
	int n_extra = 0;
	int *extra = NULL;	/* current columns that contain bad stuff */
	unsigned off = 2 + tab->M;

	extra = isl_alloc_array(tab->mat->ctx, int, tab->n_col);
	if (!extra)
		goto error;
	for (i = 0; i < tab->n_col; ++i) {
		for (j = 0; j < tab->n_col; ++j)
			if (tab->col_var[i] == col_var[j])
				break;
		if (j < tab->n_col)
			continue;
		extra[n_extra++] = i;
	}
	for (i = 0; i < tab->n_col && n_extra > 0; ++i) {
		struct isl_tab_var *var;
		int row;

		for (j = 0; j < tab->n_col; ++j)
			if (col_var[i] == tab->col_var[j])
				break;
		if (j < tab->n_col)
			continue;
		var = var_from_index(tab, col_var[i]);
		row = var->index;
		for (j = 0; j < n_extra; ++j)
			if (!isl_int_is_zero(tab->mat->row[row][off+extra[j]]))
				break;
		isl_assert(tab->mat->ctx, j < n_extra, goto error);
		if (isl_tab_pivot(tab, row, extra[j]) < 0)
			goto error;
		extra[j] = extra[--n_extra];
	}

	free(extra);
	free(col_var);
	return 0;
error:
	free(extra);
	free(col_var);
	return -1;
}

/* Remove all samples with index n or greater, i.e., those samples
 * that were added since we saved this number of samples in
 * isl_tab_save_samples.
 */
static void drop_samples_since(struct isl_tab *tab, int n)
{
	int i;

	for (i = tab->n_sample - 1; i >= 0 && tab->n_sample > n; --i) {
		if (tab->sample_index[i] < n)
			continue;

		if (i != tab->n_sample - 1) {
			int t = tab->sample_index[tab->n_sample-1];
			tab->sample_index[tab->n_sample-1] = tab->sample_index[i];
			tab->sample_index[i] = t;
			isl_mat_swap_rows(tab->samples, tab->n_sample-1, i);
		}
		tab->n_sample--;
	}
}

static int perform_undo(struct isl_tab *tab, struct isl_tab_undo *undo) WARN_UNUSED;
static int perform_undo(struct isl_tab *tab, struct isl_tab_undo *undo)
{
	switch (undo->type) {
	case isl_tab_undo_empty:
		tab->empty = 0;
		break;
	case isl_tab_undo_nonneg:
	case isl_tab_undo_redundant:
	case isl_tab_undo_freeze:
	case isl_tab_undo_zero:
	case isl_tab_undo_allocate:
	case isl_tab_undo_relax:
		return perform_undo_var(tab, undo);
	case isl_tab_undo_bset_eq:
		return isl_basic_set_free_equality(tab->bset, 1);
	case isl_tab_undo_bset_ineq:
		return isl_basic_set_free_inequality(tab->bset, 1);
	case isl_tab_undo_bset_div:
		if (isl_basic_set_free_div(tab->bset, 1) < 0)
			return -1;
		if (tab->samples)
			tab->samples->n_col--;
		break;
	case isl_tab_undo_saved_basis:
		if (restore_basis(tab, undo->u.col_var) < 0)
			return -1;
		break;
	case isl_tab_undo_drop_sample:
		tab->n_outside--;
		break;
	case isl_tab_undo_saved_samples:
		drop_samples_since(tab, undo->u.n);
		break;
	case isl_tab_undo_callback:
		return undo->u.callback->run(undo->u.callback);
	default:
		isl_assert(tab->mat->ctx, 0, return -1);
	}
	return 0;
}

/* Return the tableau to the state it was in when the snapshot "snap"
 * was taken.
 */
int isl_tab_rollback(struct isl_tab *tab, struct isl_tab_undo *snap)
{
	struct isl_tab_undo *undo, *next;

	if (!tab)
		return -1;

	tab->in_undo = 1;
	for (undo = tab->top; undo && undo != &tab->bottom; undo = next) {
		next = undo->next;
		if (undo == snap)
			break;
		if (perform_undo(tab, undo) < 0) {
			free_undo(tab);
			tab->in_undo = 0;
			return -1;
		}
		free(undo);
	}
	tab->in_undo = 0;
	tab->top = undo;
	if (!undo)
		return -1;
	return 0;
}

/* The given row "row" represents an inequality violated by all
 * points in the tableau.  Check for some special cases of such
 * separating constraints.
 * In particular, if the row has been reduced to the constant -1,
 * then we know the inequality is adjacent (but opposite) to
 * an equality in the tableau.
 * If the row has been reduced to r = -1 -r', with r' an inequality
 * of the tableau, then the inequality is adjacent (but opposite)
 * to the inequality r'.
 */
static enum isl_ineq_type separation_type(struct isl_tab *tab, unsigned row)
{
	int pos;
	unsigned off = 2 + tab->M;

	if (tab->rational)
		return isl_ineq_separate;

	if (!isl_int_is_one(tab->mat->row[row][0]))
		return isl_ineq_separate;
	if (!isl_int_is_negone(tab->mat->row[row][1]))
		return isl_ineq_separate;

	pos = isl_seq_first_non_zero(tab->mat->row[row] + off + tab->n_dead,
					tab->n_col - tab->n_dead);
	if (pos == -1)
		return isl_ineq_adj_eq;

	if (!isl_int_is_negone(tab->mat->row[row][off + tab->n_dead + pos]))
		return isl_ineq_separate;

	pos = isl_seq_first_non_zero(
			tab->mat->row[row] + off + tab->n_dead + pos + 1,
			tab->n_col - tab->n_dead - pos - 1);

	return pos == -1 ? isl_ineq_adj_ineq : isl_ineq_separate;
}

/* Check the effect of inequality "ineq" on the tableau "tab".
 * The result may be
 *	isl_ineq_redundant:	satisfied by all points in the tableau
 *	isl_ineq_separate:	satisfied by no point in the tableau
 *	isl_ineq_cut:		satisfied by some by not all points
 *	isl_ineq_adj_eq:	adjacent to an equality
 *	isl_ineq_adj_ineq:	adjacent to an inequality.
 */
enum isl_ineq_type isl_tab_ineq_type(struct isl_tab *tab, isl_int *ineq)
{
	enum isl_ineq_type type = isl_ineq_error;
	struct isl_tab_undo *snap = NULL;
	int con;
	int row;

	if (!tab)
		return isl_ineq_error;

	if (isl_tab_extend_cons(tab, 1) < 0)
		return isl_ineq_error;

	snap = isl_tab_snap(tab);

	con = isl_tab_add_row(tab, ineq);
	if (con < 0)
		goto error;

	row = tab->con[con].index;
	if (isl_tab_row_is_redundant(tab, row))
		type = isl_ineq_redundant;
	else if (isl_int_is_neg(tab->mat->row[row][1]) &&
		 (tab->rational ||
		    isl_int_abs_ge(tab->mat->row[row][1],
				   tab->mat->row[row][0]))) {
		int nonneg = at_least_zero(tab, &tab->con[con]);
		if (nonneg < 0)
			goto error;
		if (nonneg)
			type = isl_ineq_cut;
		else
			type = separation_type(tab, row);
	} else {
		int red = con_is_redundant(tab, &tab->con[con]);
		if (red < 0)
			goto error;
		if (!red)
			type = isl_ineq_cut;
		else
			type = isl_ineq_redundant;
	}

	if (isl_tab_rollback(tab, snap))
		return isl_ineq_error;
	return type;
error:
	return isl_ineq_error;
}

void isl_tab_dump(struct isl_tab *tab, FILE *out, int indent)
{
	unsigned r, c;
	int i;

	if (!tab) {
		fprintf(out, "%*snull tab\n", indent, "");
		return;
	}
	fprintf(out, "%*sn_redundant: %d, n_dead: %d", indent, "",
		tab->n_redundant, tab->n_dead);
	if (tab->rational)
		fprintf(out, ", rational");
	if (tab->empty)
		fprintf(out, ", empty");
	fprintf(out, "\n");
	fprintf(out, "%*s[", indent, "");
	for (i = 0; i < tab->n_var; ++i) {
		if (i)
			fprintf(out, (i == tab->n_param ||
				      i == tab->n_var - tab->n_div) ? "; "
								    : ", ");
		fprintf(out, "%c%d%s", tab->var[i].is_row ? 'r' : 'c',
					tab->var[i].index,
					tab->var[i].is_zero ? " [=0]" :
					tab->var[i].is_redundant ? " [R]" : "");
	}
	fprintf(out, "]\n");
	fprintf(out, "%*s[", indent, "");
	for (i = 0; i < tab->n_con; ++i) {
		if (i)
			fprintf(out, ", ");
		fprintf(out, "%c%d%s", tab->con[i].is_row ? 'r' : 'c',
					tab->con[i].index,
					tab->con[i].is_zero ? " [=0]" :
					tab->con[i].is_redundant ? " [R]" : "");
	}
	fprintf(out, "]\n");
	fprintf(out, "%*s[", indent, "");
	for (i = 0; i < tab->n_row; ++i) {
		const char *sign = "";
		if (i)
			fprintf(out, ", ");
		if (tab->row_sign) {
			if (tab->row_sign[i] == isl_tab_row_unknown)
				sign = "?";
			else if (tab->row_sign[i] == isl_tab_row_neg)
				sign = "-";
			else if (tab->row_sign[i] == isl_tab_row_pos)
				sign = "+";
			else
				sign = "+-";
		}
		fprintf(out, "r%d: %d%s%s", i, tab->row_var[i],
		    isl_tab_var_from_row(tab, i)->is_nonneg ? " [>=0]" : "", sign);
	}
	fprintf(out, "]\n");
	fprintf(out, "%*s[", indent, "");
	for (i = 0; i < tab->n_col; ++i) {
		if (i)
			fprintf(out, ", ");
		fprintf(out, "c%d: %d%s", i, tab->col_var[i],
		    var_from_col(tab, i)->is_nonneg ? " [>=0]" : "");
	}
	fprintf(out, "]\n");
	r = tab->mat->n_row;
	tab->mat->n_row = tab->n_row;
	c = tab->mat->n_col;
	tab->mat->n_col = 2 + tab->M + tab->n_col;
	isl_mat_dump(tab->mat, out, indent);
	tab->mat->n_row = r;
	tab->mat->n_col = c;
	if (tab->bset)
		isl_basic_set_dump(tab->bset, out, indent);
}
