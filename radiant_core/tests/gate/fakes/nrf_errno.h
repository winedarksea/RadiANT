/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fakes/nrf_errno.h - the two error numbers the gate names.
 *
 * Provenance: the values are nrfxlib's published ones (nrf_errno.h in
 * nrfxlib/mpsl/include), reproduced because the numbers themselves are the
 * interface: -NRF_EAGAIN is tested by value in request_work_fn() and a fake
 * that invented a different number would exercise the wrong branch.
 */

#ifndef RADIANT_CORE_TESTS_GATE_FAKE_NRF_ERRNO_H_
#define RADIANT_CORE_TESTS_GATE_FAKE_NRF_ERRNO_H_

#define NRF_EPERM  1
#define NRF_ENOMEM 12
#define NRF_EINVAL 22
#define NRF_EAGAIN 35

#endif /* RADIANT_CORE_TESTS_GATE_FAKE_NRF_ERRNO_H_ */
