// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SGP4, near-earth (sgp4.h). The variable names are the report's, so the code reads against its equations
#include <math.h>
#include "sat/sgp4.h"
#include "sat/sat_math.h"

#define PI		3.14159265358979323846
#define TWOPI		(2.0 * PI)
#define DEG2RAD		(PI / 180.0)

// WGS-72
#define MU		398600.8		// km^3 / s^2
#define RE		6378.135		// km
#define J2		0.001082616
#define J3		-0.00000253881
#define J4		-0.00000165597
#define J3OJ2		(J3 / J2)
#define X2O3		(2.0 / 3.0)

// Earth radii per minute: 60 / sqrt(RE^3 / MU)
static double xke(void)
{
	return 60.0 / sqrt(RE * RE * RE / MU);
}

// x^(2/3) for the semi-major axis from the mean motion. Newton's method for the cube root instead of pow(), which
// brings 3 KB of the C library; a near-earth orbit's x is 0.5 to 3, where it converges from 1 in under 8 steps
static double pow2o3(double x)
{
	double y = 1.0;
	for (int i = 0; i < 40; i++) {
		double dy = (y - x / (y * y)) / 3.0;
		y -= dy;
		if (fabs(dy) <= 1e-16 * y)
			break;
	}
	return y * y;
}

// An angle less whole turns, its sign kept (as fmod(x, 2 pi), without the C library's 0.8 KB)
static double rev(double x)
{
	return x - TWOPI * (double)(int32_t)(x / TWOPI);
}

sgp4_err_t sgp4_init(sgp4_t *s, const sat_elements_t *e)
{
	const double k = xke();
	s->epoch = e->epoch;
	s->bstar = e->bstar;
	s->inclo = e->inclo * DEG2RAD;
	s->nodeo = e->nodeo * DEG2RAD;
	s->argpo = e->argpo * DEG2RAD;
	s->mo = e->mo * DEG2RAD;
	s->ecco = e->ecco;
	if (e->ecco < 0.0 || e->ecco >= 1.0)
		return SGP4_ERR_ECC;
	double no_kozai = e->no * TWOPI / 1440.0;
	if (no_kozai <= 0.0)
		return SGP4_ERR_MEAN_MOTION;

	// Recover the original mean motion and semi-major axis from the Kozai mean motion the elements give
	double eccsq = s->ecco * s->ecco;
	double omeosq = 1.0 - eccsq;
	double rteosq = sqrt(omeosq);
	double cosio = sat_cos(s->inclo);
	double cosio2 = cosio * cosio;
	double ak = pow2o3(k / no_kozai);
	double d1 = 0.75 * J2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
	double del = d1 / (ak * ak);
	double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
	del = d1 / (adel * adel);
	s->no = no_kozai / (1.0 + del);
	double ao = pow2o3(k / s->no);
	s->sinio = sat_sin(s->inclo);
	s->cosio = cosio;
	double po = ao * omeosq;
	double con42 = 1.0 - 5.0 * cosio2;
	s->con41 = -con42 - cosio2 - cosio2;
	double posq = po * po;
	double rp = ao * (1.0 - s->ecco);
	if (TWOPI / s->no >= 225.0)
		return SGP4_ERR_DEEP_SPACE;

	// Perigee below 220 km: the drag terms past the first are left out
	s->isimp = rp < 220.0 / RE + 1.0;
	// The atmosphere's density parameter s and (q0 - s)^4, lowered for a perigee under 156 km
	double sfour = 78.0 / RE + 1.0;
	double q = (120.0 - 78.0) / RE;
	double qzms24 = q * q * q * q;
	double perige = (rp - 1.0) * RE;
	if (perige < 156.0) {
		sfour = perige - 78.0;
		if (perige < 98.0)
			sfour = 20.0;
		q = (120.0 - sfour) / RE;
		qzms24 = q * q * q * q;
		sfour = sfour / RE + 1.0;
	}
	double pinvsq = 1.0 / posq;
	double tsi = 1.0 / (ao - sfour);
	s->eta = ao * s->ecco * tsi;
	double etasq = s->eta * s->eta;
	double eeta = s->ecco * s->eta;
	double psisq = fabs(1.0 - etasq);
	double coef = qzms24 * tsi * tsi * tsi * tsi;
	double coef1 = coef / (psisq * psisq * psisq * sqrt(psisq));
	double cc2 = coef1 * s->no * (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
		0.375 * J2 * tsi / psisq * s->con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
	s->cc1 = s->bstar * cc2;
	double cc3 = 0.0;
	if (s->ecco > 1.0e-4)
		cc3 = -2.0 * coef * tsi * J3OJ2 * s->no * s->sinio / s->ecco;
	s->x1mth2 = 1.0 - cosio2;
	s->cc4 = 2.0 * s->no * coef1 * ao * omeosq * (s->eta * (2.0 + 0.5 * etasq) + s->ecco * (0.5 + 2.0 * etasq) -
		J2 * tsi / (ao * psisq) * (-3.0 * s->con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
		0.75 * s->x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * sat_cos(2.0 * s->argpo)));
	s->cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);

	// Secular rates of the mean anomaly, perigee and node from J2 and J4
	double cosio4 = cosio2 * cosio2;
	double temp1 = 1.5 * J2 * pinvsq * s->no;
	double temp2 = 0.5 * temp1 * J2 * pinvsq;
	double temp3 = -0.46875 * J4 * pinvsq * pinvsq * s->no;
	s->mdot = s->no + 0.5 * temp1 * rteosq * s->con41 + 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
	s->argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
		temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
	double xhdot1 = -temp1 * cosio;
	s->nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
	s->omgcof = s->bstar * cc3 * sat_cos(s->argpo);
	s->xmcof = 0.0;
	if (s->ecco > 1.0e-4)
		s->xmcof = -X2O3 * coef * s->bstar / eeta;
	s->nodecf = 3.5 * omeosq * xhdot1 * s->cc1;
	s->t2cof = 1.5 * s->cc1;
	// The long-period term divides by 1 + cos i: kept finite for a retrograde equatorial orbit
	double opc = fabs(cosio + 1.0) > 1.5e-12 ? 1.0 + cosio : 1.5e-12;
	s->xlcof = -0.25 * J3OJ2 * s->sinio * (3.0 + 5.0 * cosio) / opc;
	s->aycof = -0.5 * J3OJ2 * s->sinio;
	double dm = 1.0 + s->eta * sat_cos(s->mo);
	s->delmo = dm * dm * dm;
	s->sinmao = sat_sin(s->mo);
	s->x7thm1 = 7.0 * cosio2 - 1.0;

	s->d2 = s->d3 = s->d4 = s->t3cof = s->t4cof = s->t5cof = 0.0;
	if (!s->isimp) {
		double cc1sq = s->cc1 * s->cc1;
		s->d2 = 4.0 * ao * tsi * cc1sq;
		double temp = s->d2 * tsi * s->cc1 / 3.0;
		s->d3 = (17.0 * ao + sfour) * temp;
		s->d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * s->cc1;
		s->t3cof = s->d2 + 2.0 * cc1sq;
		s->t4cof = 0.25 * (3.0 * s->d3 + s->cc1 * (12.0 * s->d2 + 10.0 * cc1sq));
		s->t5cof = 0.2 * (3.0 * s->d4 + 12.0 * s->cc1 * s->d3 + 6.0 * s->d2 * s->d2 + 15.0 * cc1sq * (2.0 * s->d2 + cc1sq));
	}

	// The report runs the model once at epoch, which catches elements that decay at once
	sgp4_state_t st;
	return sgp4_propagate(s, 0.0, &st);
}

sgp4_err_t sgp4_propagate(const sgp4_t *s, double t, sgp4_state_t *out)
{
	const double k = xke();

	// Secular gravity and drag
	double xmdf = s->mo + s->mdot * t;
	double argpdf = s->argpo + s->argpdot * t;
	double nodedf = s->nodeo + s->nodedot * t;
	double argpm = argpdf;
	double mm = xmdf;
	double t2 = t * t;
	double nodem = nodedf + s->nodecf * t2;
	double tempa = 1.0 - s->cc1 * t;
	double tempe = s->bstar * s->cc4 * t;
	double templ = s->t2cof * t2;
	if (!s->isimp) {
		double delomg = s->omgcof * t;
		double dm = 1.0 + s->eta * sat_cos(xmdf);
		double delm = s->xmcof * (dm * dm * dm - s->delmo);
		double temp = delomg + delm;
		mm = xmdf + temp;
		argpm = argpdf - temp;
		double t3 = t2 * t;
		double t4 = t3 * t;
		tempa = tempa - s->d2 * t2 - s->d3 * t3 - s->d4 * t4;
		tempe = tempe + s->bstar * s->cc5 * (sat_sin(mm) - s->sinmao);
		templ = templ + s->t3cof * t3 + t4 * (s->t4cof + t * s->t5cof);
	}
	double nm = s->no;
	double em = s->ecco;
	if (nm <= 0.0)
		return SGP4_ERR_MEAN_MOTION;
	double am = pow2o3(k / nm) * tempa * tempa;
	nm = k / (am * sqrt(am));
	em = em - tempe;
	if (em >= 1.0 || em < -0.001)
		return SGP4_ERR_ECC;
	if (em < 1.0e-6)
		em = 1.0e-6;
	mm = mm + s->no * templ;
	double xlm = mm + argpm + nodem;
	nodem = rev(nodem);
	argpm = rev(argpm);
	xlm = rev(xlm);

	// Long-period periodics
	double sinargp, cosargp;
	sat_sincos(argpm, &sinargp, &cosargp);
	double axnl = em * cosargp;
	double temp = 1.0 / (am * (1.0 - em * em));
	double aynl = em * sinargp + temp * s->aycof;
	double xl = xlm + temp * s->xlcof * axnl;

	// Kepler's equation for E + w, Newton's method with the step held to 0.95 rad
	double u = rev(xl - nodem);
	double eo1 = u;
	double sineo1 = 0.0, coseo1 = 1.0;
	double tem5 = 1.0;
	for (int ktr = 0; fabs(tem5) >= 1.0e-12 && ktr < 10; ktr++) {
		sat_sincos(eo1, &sineo1, &coseo1);
		tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
		tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
		if (fabs(tem5) >= 0.95)
			tem5 = tem5 > 0.0 ? 0.95 : -0.95;
		eo1 += tem5;
	}

	// Short-period preliminaries
	double ecose = axnl * coseo1 + aynl * sineo1;
	double esine = axnl * sineo1 - aynl * coseo1;
	double el2 = axnl * axnl + aynl * aynl;
	double pl = am * (1.0 - el2);
	if (pl < 0.0)
		return SGP4_ERR_SEMI_LATUS;
	double rl = am * (1.0 - ecose);
	double rdotl = sqrt(am) * esine / rl;
	double rvdotl = sqrt(pl) / rl;
	double betal = sqrt(1.0 - el2);
	temp = esine / (1.0 + betal);
	double sinu = am / rl * (sineo1 - aynl - axnl * temp);
	double cosu = am / rl * (coseo1 - axnl + aynl * temp);
	double su = atan2(sinu, cosu);
	double sin2u = (cosu + cosu) * sinu;
	double cos2u = 1.0 - 2.0 * sinu * sinu;
	temp = 1.0 / pl;
	double temp1 = 0.5 * J2 * temp;
	double temp2 = temp1 * temp;

	// Short-period periodics
	double mrt = rl * (1.0 - 1.5 * temp2 * betal * s->con41) + 0.5 * temp1 * s->x1mth2 * cos2u;
	su = su - 0.25 * temp2 * s->x7thm1 * sin2u;
	double xnode = nodem + 1.5 * temp2 * s->cosio * sin2u;
	double xinc = s->inclo + 1.5 * temp2 * s->cosio * s->sinio * cos2u;
	double mvt = rdotl - nm * temp1 * s->x1mth2 * sin2u / k;
	double rvdot = rvdotl + nm * temp1 * (s->x1mth2 * cos2u + 1.5 * s->con41) / k;

	// Orientation vectors
	double sinsu, cossu, snod, cnod, sini, cosi;
	sat_sincos(su, &sinsu, &cossu);
	sat_sincos(xnode, &snod, &cnod);
	sat_sincos(xinc, &sini, &cosi);
	double xmx = -snod * cosi;
	double xmy = cnod * cosi;
	double ux = xmx * sinsu + cnod * cossu;
	double uy = xmy * sinsu + snod * cossu;
	double uz = sini * sinsu;
	double vx = xmx * cossu - cnod * sinsu;
	double vy = xmy * cossu - snod * sinsu;
	double vz = sini * cossu;

	if (mrt < 1.0)
		return SGP4_ERR_DECAYED;
	const double vkms = RE * k / 60.0;
	out->r[0] = mrt * ux * RE;
	out->r[1] = mrt * uy * RE;
	out->r[2] = mrt * uz * RE;
	out->v[0] = (mvt * ux + rvdot * vx) * vkms;
	out->v[1] = (mvt * uy + rvdot * vy) * vkms;
	out->v[2] = (mvt * uz + rvdot * vz) * vkms;
	return SGP4_OK;
}
