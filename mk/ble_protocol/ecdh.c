#include <string.h>

#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"

#include "crypto.h"
#include "ecdh.h"
#include "state.h"

static mbedtls_ecp_group ec_grp;
static mbedtls_mpi       ec_d;
static mbedtls_ecp_point ec_Q;

void ecdh_init(void) {
    mbedtls_ecp_group_init(&ec_grp);
    mbedtls_mpi_init(&ec_d);
    mbedtls_ecp_point_init(&ec_Q);
}

int ecdh_make_dev_keys(void) {
    mbedtls_ecp_group_free(&ec_grp);
    mbedtls_ecp_group_init(&ec_grp);

    mbedtls_mpi_free(&ec_d);
    mbedtls_mpi_init(&ec_d);

    mbedtls_ecp_point_free(&ec_Q);
    mbedtls_ecp_point_init(&ec_Q);

    int rc = mbedtls_ecp_group_load(&ec_grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc != 0) return rc;

    rc = mbedtls_ecdh_gen_public(&ec_grp, &ec_d, &ec_Q, my_rng, NULL);
    if (rc != 0) return rc;

    size_t olen = 0;
    uint8_t tmp[80];
    rc = mbedtls_ecp_point_write_binary(&ec_grp, &ec_Q,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen, tmp, sizeof(tmp));
    if (rc != 0) return rc;
    if (olen != 65) return -1;

    memcpy(dev_pub65, tmp, 65);
    return 0;
}

int ecdh_compute_shared_secret(const uint8_t host_pub[65], uint8_t out_secret32[32]) {
    mbedtls_ecp_point Qp;
    mbedtls_ecp_point_init(&Qp);

    int rc = mbedtls_ecp_point_read_binary(&ec_grp, &Qp, host_pub, 65);
    if (rc != 0) { mbedtls_ecp_point_free(&Qp); return rc; }

    mbedtls_mpi z;
    mbedtls_mpi_init(&z);

    rc = mbedtls_ecdh_compute_shared(&ec_grp, &z, &Qp, &ec_d, my_rng, NULL);
    if (rc != 0) {
        mbedtls_mpi_free(&z);
        mbedtls_ecp_point_free(&Qp);
        return rc;
    }

    rc = mbedtls_mpi_write_binary(&z, out_secret32, 32);

    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&Qp);
    return rc;
}
