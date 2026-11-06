// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pass prediction (sat_pass.h)
#include "sat/sat_pass.h"

// Below the horizon, a low earth orbit's elevation changes by less than 0.5 degree a second (about 0.3 at the horizon
// for a 200 km orbit, the lowest), so a step of 2 s per degree below can't step over a rise. Held to 20..300 s: a pass
// shorter than 20 s peaks a fraction of a degree up, and no one works it
#define STEP_PER_DEG	2.0
#define STEP_MIN	20.0
#define STEP_MAX	300.0
// The step through a pass, looking for its highest point and its set
#define STEP_IN_PASS	20.0
// Rise and set to the second
#define BISECT_TO	1.0
// A pass in progress began at most this long before `from`: no low earth orbit pass lasts longer
#define PASS_MAX	(30.0 * 60.0)

typedef struct {
	const sgp4_t *s;
	const sat_observer_t *o;
	unsigned evals;
	bool failed;
} search_t;

static double elev(search_t *q, double t, sat_look_t *l)
{
	sat_look_t tmp;
	if (l == NULL)
		l = &tmp;
	q->evals++;
	if (!sat_look_at(q->s, q->o, t, l)) {
		q->failed = true;
		l->el = -90.0;
		l->az = 0.0;
	}
	return l->el;
}

// The horizon crossing between lo (below) and hi (above), or the other way round for a set
static double crossing(search_t *q, double below, double above)
{
	while (above - below > BISECT_TO || below - above > BISECT_TO) {
		double mid = 0.5 * (below + above);
		if (elev(q, mid, NULL) > 0.0)
			above = mid;
		else
			below = mid;
	}
	return above;
}

// The highest point between a and b, which bracket a single maximum: golden-section search to the second
static double highest(search_t *q, double a, double b)
{
	const double g = 0.6180339887498949;
	double c = b - g * (b - a), d = a + g * (b - a);
	double fc = elev(q, c, NULL), fd = elev(q, d, NULL);
	while (b - a > BISECT_TO) {
		if (fc > fd) {
			b = d;
			d = c;
			fd = fc;
			c = b - g * (b - a);
			fc = elev(q, c, NULL);
		} else {
			a = c;
			c = d;
			fc = fd;
			d = a + g * (b - a);
			fd = elev(q, d, NULL);
		}
	}
	return 0.5 * (a + b);
}

static double below_step(double el)
{
	double dt = -el * STEP_PER_DEG;
	return dt < STEP_MIN ? STEP_MIN : dt > STEP_MAX ? STEP_MAX : dt;
}

bool sat_pass_next(const sgp4_t *s, const sat_observer_t *o, double from, double span, float min_el, sat_pass_t *out,
	unsigned *evals)
{
	search_t q = { .s = s, .o = o };
	double end = from + span;
	double t = from;
	bool found = false;
	double el = elev(&q, t, NULL);

	// Up already: walk back to the rise of the pass in progress
	double aos = 0.0;
	bool up = el > 0.0;
	if (up) {
		double back = t;
		while (elev(&q, back - STEP_IN_PASS, NULL) > 0.0 && t - back < PASS_MAX)
			back -= STEP_IN_PASS;
		aos = crossing(&q, back - STEP_IN_PASS, back);
		// Its highest point may lie behind `from`: scan from the rise
		t = aos;
		el = elev(&q, t, NULL);
	}

	while (!q.failed && t <= end) {
		if (!up) {
			double prev = t;
			t += below_step(el);
			el = elev(&q, t, NULL);
			if (el <= 0.0)
				continue;
			aos = crossing(&q, prev, t);
		}
		up = false;

		// Through the pass to the set, keeping the highest sample
		sat_look_t rise;
		elev(&q, aos, &rise);
		double best_t = t, best_el = el, prev = t;
		while (el > 0.0 && !q.failed) {
			prev = t;
			t += STEP_IN_PASS;
			el = elev(&q, t, NULL);
			if (el > best_el) {
				best_el = el;
				best_t = t;
			}
		}
		double los = crossing(&q, t, prev);
		double lo = best_t - STEP_IN_PASS < aos ? aos : best_t - STEP_IN_PASS;
		double hi = best_t + STEP_IN_PASS > los ? los : best_t + STEP_IN_PASS;
		double tca = highest(&q, lo, hi);
		sat_look_t top, set;
		elev(&q, tca, &top);
		elev(&q, los, &set);
		if (q.failed)
			break;
		if (top.el >= (double)min_el && aos <= end) {
			out->aos = aos;
			out->tca = tca;
			out->los = los;
			out->max_el = (float)top.el;
			out->aos_az = (float)rise.az;
			out->los_az = (float)set.az;
			found = true;
			break;
		}
		// Too low: on from after the set
		t = los + STEP_MIN;
		el = elev(&q, t, NULL);
	}
	if (evals != NULL)
		*evals = q.evals;
	return found;
}
