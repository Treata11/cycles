/* SPDX-FileCopyrightText: 2011-2024 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

CCL_NAMESPACE_BEGIN

/* Given a random number, sample a direction that makes an angle of theta with direction D. */
ccl_device float3 phase_sample_direction(float3 D, float cos_theta, float rand)
{
  float sin_theta = sin_from_cos(cos_theta);
  float phi = M_2PI_F * rand;
  float3 dir = make_float3(sin_theta * cosf(phi), sin_theta * sinf(phi), cos_theta);

  float3 T, B;
  make_orthonormals(D, &T, &B);
  dir = dir.x * T + dir.y * B + dir.z * D;

  return dir;
}

/* Given cosine between rays, return probability density that a photon bounces
 * to that direction. The g parameter controls how different it is from the
 * uniform sphere. g=0 uniform diffuse-like, g=1 close to sharp single ray. */
ccl_device float phase_henyey_greenstein(float cos_theta, float g)
{
  if (fabsf(g) < 1e-3f) {
    return M_1_4PI_F;
  }
  const float fac = 1 + g * (g - 2 * cos_theta);
  return (1 - sqr(g)) / (M_4PI_F * fac * safe_sqrtf(fac));
}

ccl_device float3 phase_henyey_greenstein_sample(float3 D,
                                                 float g,
                                                 float2 rand,
                                                 ccl_private float *pdf)
{
  float cos_theta = 1 - 2 * rand.x;
  if (fabsf(g) >= 1e-3f) {
    const float k = (1 - sqr(g)) / (1 - g * cos_theta);
    cos_theta = (1 + sqr(g) - sqr(k)) / (2 * g);
  }

  *pdf = phase_henyey_greenstein(cos_theta, g);

  return phase_sample_direction(D, cos_theta, rand.y);
}

/* Given cosine between rays, return probability density that a photon bounces to that direction
 * according to the constant Rayleigh phase function.
 * See https://doi.org/10.1364/JOSAA.28.002436 for details. */
ccl_device float phase_rayleigh(float cos_theta)
{
  return (0.1875f * M_1_PI_F) * (1.0f + sqr(cos_theta));
}

ccl_device float3 phase_rayleigh_sample(float3 D, float2 rand, ccl_private float *pdf)
{
  const float a = 2 - 4 * rand.x;
  /* Metal doesn't have cbrtf, but since we compute u - 1/u anyways, we can just as well
   * use the inverse cube root for which there is a simple Quake-style fast implementation.*/
  const float inv_u = -fast_inv_cbrtf(sqrtf(1 + sqr(a)) + a);
  const float cos_theta = 1 / inv_u - inv_u;
  *pdf = phase_rayleigh(cos_theta);

  return phase_sample_direction(D, cos_theta, rand.y);
}

/* Given cosine between rays, return probability density that a photon bounces to that direction
 * according to the Draine phase function. This is a generalization of the Henyey-Greenstein
 * function which bridges the cases of HG and Rayleigh scattering. The parameter g mainly controls
 * the first moment <cos theta>, and alpha the second moment <cos2 theta> of the exact phase
 * function. alpha=0 reduces to HG function, g=0, alpha=1 reduces to Rayleigh function, alpha=1
 * reduces to Cornette-Shanks function.
 * See https://doi.org/10.1086/379118 for details. */
ccl_device float phase_draine(float cos_theta, float g, float alpha)
{
  /* Check special cases. */
  if (fabsf(g) < 1e-3f && alpha > 0.999f) {
    return phase_rayleigh(cos_theta);
  }
  else if (fabsf(alpha) < 1e-3f) {
    return phase_henyey_greenstein(cos_theta, g);
  }

  const float g2 = sqr(g);
  const float fac = 1 + g2 - 2 * g * cos_theta;
  return ((1 - g2) * (1 + alpha * sqr(cos_theta))) /
         ((1 + (alpha * (1 + 2 * g2)) * (1 / 3.0f)) * M_4PI_F * fac * sqrtf(fac));
}

/* Adapted from the HLSL code provided in https://research.nvidia.com/labs/rtr/approximate-mie/ */
ccl_device float phase_draine_sample_cos(float g, float alpha, float rand)
{
  if (fabsf(g) < 1e-2f) {
    /* Special case to prevent division by zero.
     * The sample technique is similar as in https://doi.org/10.1364/JOSAA.28.002436. */
    const float inv_alpha = 1.0f / alpha;
    const float b_2 = (3 + alpha) * inv_alpha * (0.5f - rand);
    const float inv_u = -fast_inv_cbrtf(b_2 + sqrtf(sqr(b_2) + sqr(inv_alpha) * inv_alpha));
    return 1 / inv_u - inv_u / alpha;
  }
  const float g2 = sqr(g), g3 = g * g2, g4 = sqr(g2), g6 = g2 * g4;
  const float pgp1_2 = sqr(1 + g2);
  const float T1a = alpha * (g4 - 1), T1a3 = sqr(T1a) * T1a;
  const float T2 = -1296 * (g2 - 1) * (alpha - alpha * g2) * T1a * (4 * g2 + alpha * pgp1_2);
  const float T9 = 2 + g2 + g3 * (1 + 2 * g2) * (2 * rand - 1);
  const float T3 = 3 * g2 * (1 + g * (2 * rand - 1)) + alpha * T9;
  const float T4a = 432 * T1a3 + T2 + 432 * (alpha * (1 - g2)) * sqr(T3);
  const float T10 = alpha * (2 * g4 - g2 - g6);
  const float T4b = 144 * T10, T4b3 = sqr(T4b) * T4b;
  const float T4 = T4a + sqrtf(-4 * T4b3 + sqr(T4a));
  const float inv_T4p3 = fast_inv_cbrtf(T4);
  const float T8 = 48 * M_CBRT2_F * T10;
  const float T6 = (2 * T1a + T8 * inv_T4p3 + 1 / (3 * M_CBRT2_F * inv_T4p3)) / (alpha * (1 - g2));
  const float T5 = 6 * (1 + g2) + T6;
  const float T7 = 6 * (1 + g2) - (8 * T3) / (alpha * (g2 - 1) * sqrtf(T5)) - T6;
  return (1 + g2 - 0.25f * sqr(sqrtf(T7) - sqrtf(T5))) / (2 * g);
}

ccl_device float3
phase_draine_sample(float3 D, float g, float alpha, float2 rand, ccl_private float *pdf)
{
  /* Check special cases. */
  if (fabsf(g) < 1e-3f && alpha > 0.999f) {
    return phase_rayleigh_sample(D, rand, pdf);
  }
  else if (fabsf(alpha) < 1e-3f) {
    return phase_henyey_greenstein_sample(D, g, rand, pdf);
  }

  const float cos_theta = phase_draine_sample_cos(g, alpha, rand.x);
  *pdf = phase_draine(cos_theta, g, alpha);

  return phase_sample_direction(D, cos_theta, rand.y);
}

ccl_device float phase_fournier_forand_delta(float n, float sin_htheta_sqr)
{
  float u = 4 * sin_htheta_sqr;
  return u / (3 * sqr(n - 1));
}

ccl_device_inline float3 phase_fournier_forand_coeffs(float B, float IOR)
{
  const float d90 = phase_fournier_forand_delta(IOR, 0.5f);
  const float d180 = phase_fournier_forand_delta(IOR, 1.0f);
  const float v = -logf(2 * B * (d90 - 1) + 1) / logf(d90);
  return make_float3(IOR, v, (powf(d180, -v) - 1) / (d180 - 1));
}

/* Given cosine between rays, return probability density that a photon bounces to that direction
 * according to the Fournier-Forand phase function. The n parameter is the particle index of
 * refraction and controls how much of the light is refracted. B is the particle backscatter
 * fraction, B = b_b / b.
 * See https://doi.org/10.1117/12.366488 for details. */
ccl_device_inline float phase_fournier_forand_impl(
    float cos_theta, float delta, float pow_delta_v, float v, float sin_htheta_sqr, float pf_coeff)
{
  const float m_delta = 1 - delta, m_pow_delta_v = 1 - pow_delta_v;

  float pf;
  if (fabsf(m_delta) < 1e-3f) {
    /* Special case (first-order Taylor expansion) to avoid singularity at delta near 1.0 */
    pf = v * ((v - 1) - (v + 1) / sin_htheta_sqr) * (1 / (8 * M_PI_F));
    pf += v * (v + 1) * m_delta * (2 * (v - 1) - (2 * v + 1) / sin_htheta_sqr) *
          (1 / (24 * M_PI_F));
  }
  else {
    pf = (v * m_delta - m_pow_delta_v + (delta * m_pow_delta_v - v * m_delta) / sin_htheta_sqr) /
         (M_4PI_F * sqr(m_delta) * pow_delta_v);
  }
  pf += pf_coeff * (3 * sqr(cos_theta) - 1);
  return pf;
}

ccl_device float phase_fournier_forand(float cos_theta, float3 coeffs)
{
  if (fabsf(cos_theta) >= 1.0f) {
    return 0.0f;
  }

  const float n = coeffs.x, v = coeffs.y;
  const float pf_coeff = coeffs.z * (1.0f / (16.0f * M_PI_F));
  const float sin_htheta_sqr = 0.5f * (1 - cos_theta); /* sin^2(theta / 2)*/
  const float delta = phase_fournier_forand_delta(n, sin_htheta_sqr);

  return phase_fournier_forand_impl(cos_theta, delta, powf(delta, v), v, sin_htheta_sqr, pf_coeff);
}

ccl_device float phase_fournier_forand_newton(float rand, float3 coeffs)
{
  const float n = coeffs.x, v = coeffs.y;
  const float cdf_coeff = coeffs.z * (1.0f / 8.0f);
  const float pf_coeff = coeffs.z * (1.0f / (16.0f * M_PI_F));

  float cos_theta = 0.64278760968f; /* Initial guess: 50 degrees */
  for (int it = 0; it < 20; it++) {
    const float sin_htheta_sqr = 0.5f * (1 - cos_theta); /* sin^2(theta / 2)*/
    const float delta = phase_fournier_forand_delta(n, sin_htheta_sqr);
    const float pow_delta_v = powf(delta, v);
    const float m_delta = 1 - delta, m_pow_delta_v = 1 - pow_delta_v;

    /* Evaluate CDF and phase functions */
    float cdf;
    if (fabsf(m_delta) < 1e-3f) {
      /* Special case (first-order Taylor expansion) to avoid singularity at delta near 1.0 */
      cdf = 1 + v * (1 - sin_htheta_sqr) * (1 - 0.5f * (v + 1) * m_delta);
    }
    else {
      cdf = (1 - pow_delta_v * delta - m_pow_delta_v * sin_htheta_sqr) / (m_delta * pow_delta_v);
    }
    cdf += cdf_coeff * cos_theta * (1 - sqr(cos_theta));
    const float pf = phase_fournier_forand_impl(
        cos_theta, delta, pow_delta_v, v, sin_htheta_sqr, pf_coeff);

    /* Perform Newton iteration step */
    float new_cos_theta = cos_theta + M_1_2PI_F * (cdf - rand) / pf;

    /* Don't step off past 1.0, approach the peak slowly */
    if (new_cos_theta >= 1.0f) {
      new_cos_theta = max(mix(cos_theta, 1.0f, 0.5f), 0.99f);
    }
    if (fabsf(cos_theta - new_cos_theta) < 1e-6f || new_cos_theta == 1.0f) {
      return new_cos_theta;
    }
    cos_theta = new_cos_theta;
  }
  /* Reached iteration limit, so give up and use what we have. */
  return cos_theta;
}

ccl_device float3 phase_fournier_forand_sample(float3 D,
                                               float3 coeffs,
                                               float2 rand,
                                               ccl_private float *pdf)
{
  float cos_theta = phase_fournier_forand_newton(rand.x, coeffs);
  *pdf = phase_fournier_forand(cos_theta, coeffs);

  return phase_sample_direction(D, cos_theta, rand.y);
}

CCL_NAMESPACE_END