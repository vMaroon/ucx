/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2021. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "ucp_test.h"
#include <ucp/core/ucp_context.h>

extern "C" {
#include <ucp/core/ucp_ep.inl>
#include <ucp/core/ucp_worker.inl>
}

#include <map>

class test_ucp_ep : public ucp_test {
public:
    static void get_test_variants(std::vector<ucp_test_variant> &variants)
    {
        add_variant(variants, UCP_FEATURE_TAG);
    }

    /// @override
    virtual void init()
    {
        ucp_test::init();
        sender().connect(&receiver(), get_ep_params());
    }
};

UCS_TEST_P(test_ucp_ep, ucp_query_ep)
{
    ucp_ep_h ep;
    ucs_status_t status;
    ucp_ep_evaluate_perf_param_t param;
    ucp_ep_evaluate_perf_attr_t attr;
    double estimated_time_0, estimated_time_1000;

    param.field_mask   = UCP_EP_PERF_PARAM_FIELD_MESSAGE_SIZE;
    attr.field_mask    = UCP_EP_PERF_ATTR_FIELD_ESTIMATED_TIME;
    param.message_size = 0;
    create_entity();

    ep     = sender().ep();
    status = ucp_ep_evaluate_perf(ep, &param, &attr);

    EXPECT_EQ(status, UCS_OK);
    EXPECT_GE(attr.estimated_time, 0);
    estimated_time_0 = attr.estimated_time;

    param.message_size = 1000;
    status             = ucp_ep_evaluate_perf(ep, &param, &attr);
    EXPECT_EQ(status, UCS_OK);
    EXPECT_GT(attr.estimated_time, 0);
    EXPECT_LT(attr.estimated_time, 10);
    estimated_time_1000 = attr.estimated_time;

    param.message_size = 2000;
    status             = ucp_ep_evaluate_perf(ep, &param, &attr);
    EXPECT_EQ(status, UCS_OK);
    EXPECT_GT(attr.estimated_time, 0);
    EXPECT_LT(attr.estimated_time, 10);

    /* Test time estimation sanity, by verifying constant increase per message
       size (which represents current calculation model) */
    EXPECT_FLOAT_EQ(attr.estimated_time - estimated_time_1000,
                    estimated_time_1000 - estimated_time_0);
}

UCS_TEST_P(test_ucp_ep, ucp_query_transport)
{
    ucs_status_t status;
    int i;
    ucp_ep_attr_t ep_attrs;
    std::vector<ucp_transport_entry_t> transport_entries(100);

    ep_attrs.field_mask             = UCP_EP_ATTR_FIELD_TRANSPORTS;
    ep_attrs.transports.entries     = &transport_entries[0];
    ep_attrs.transports.num_entries = transport_entries.size();
    ep_attrs.transports.entry_size  = sizeof(ucp_transport_entry_t);
    status                          = ucp_ep_query(sender().ep(), &ep_attrs);

    // Verify ucp_ep_query completed successfully, that the number of
    // transports is updated, since no system should reasonably have
    // 100 transport/device name pairs, and verify returned strings are
    // not empty.
    ASSERT_UCS_OK(status);
    EXPECT_LT(ep_attrs.transports.num_entries, 100);
    UCS_TEST_MESSAGE << "Number of transport/device name pairs: "
                     << ep_attrs.transports.num_entries;
    for (i = 0; i < ep_attrs.transports.num_entries; i++) {
        UCS_TEST_MESSAGE << "Transport[" << i << "] transport="
                         << ep_attrs.transports.entries[i].transport_name
                         << " device="
                         << ep_attrs.transports.entries[i].device_name;
        EXPECT_STRNE(ep_attrs.transports.entries[i].transport_name, "");
        EXPECT_STRNE(ep_attrs.transports.entries[i].device_name, "");
    }
}

UCP_INSTANTIATE_TEST_CASE(test_ucp_ep);


class test_ucp_ep_connect_fail : public ucp_test {
public:
    static void get_test_variants(std::vector<ucp_test_variant> &variants)
    {
        add_variant(variants, UCP_FEATURE_TAG);
    }

protected:
    virtual void cleanup()
    {
        m_mock.cleanup();
        m_orig_ep_destroy.clear();
        ucp_test::cleanup();
    }

    /* Fail every transport endpoint creation after the first one */
    static ucs_status_t
    ep_create_fail_after_first(const uct_ep_params_t *params, uct_ep_h *ep_p)
    {
        if (m_num_created > 0) {
            return UCS_ERR_NO_RESOURCE;
        }

        UCS_MOCK_ORIG_FUNC(m_mock, &params->iface->ops.ep_create, params,
                           ep_p);
        ++m_num_created;
        return UCS_OK;
    }

    static void ep_destroy_count(uct_ep_h uct_ep)
    {
        ++m_num_destroyed;
        m_orig_ep_destroy.at(uct_ep->iface)(uct_ep);
    }

    void mock_ifaces(ucp_worker_h worker)
    {
        for (ucp_rsc_index_t rsc_index = 0;
             rsc_index < worker->context->num_tls; ++rsc_index) {
            ucp_worker_iface_t *wiface = ucp_worker_iface(worker, rsc_index);
            if ((wiface == NULL) || (wiface->iface == NULL)) {
                continue;
            }

            /* ucs::mock::orig_func() does not support void functions */
            uct_iface_h iface        = wiface->iface;
            m_orig_ep_destroy[iface] = iface->ops.ep_destroy;
            m_mock.setup(&iface->ops.ep_create, ep_create_fail_after_first);
            m_mock.setup(&iface->ops.ep_destroy, ep_destroy_count);
        }
    }

    static ucs::mock m_mock;
    static unsigned m_num_created;
    static unsigned m_num_destroyed;
    static std::map<uct_iface_h, uct_ep_destroy_func_t> m_orig_ep_destroy;
};

ucs::mock test_ucp_ep_connect_fail::m_mock;
unsigned test_ucp_ep_connect_fail::m_num_created   = 0;
unsigned test_ucp_ep_connect_fail::m_num_destroyed = 0;
std::map<uct_iface_h, uct_ep_destroy_func_t>
        test_ucp_ep_connect_fail::m_orig_ep_destroy;

UCS_TEST_P(test_ucp_ep_connect_fail, lane_failure_destroys_connected_lanes)
{
    ucp_ep_params_t ep_params = get_ep_params();
    ucp_address_t *address;
    size_t address_length;
    ucs_status_t status;
    ucp_ep_h ep;

    sender().connect(&receiver(), ep_params);
    if (ucp_ep_num_lanes(sender().ep()) < 2) {
        UCS_TEST_SKIP_R("endpoint has less than 2 lanes");
    }

    m_num_created   = 0;
    m_num_destroyed = 0;
    mock_ifaces(sender().worker());

    ASSERT_UCS_OK(ucp_worker_get_address(receiver().worker(), &address,
                                         &address_length));
    ep_params.field_mask |= UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    ep_params.address     = address;
    {
        scoped_log_handler slh(hide_errors_logger);
        status = ucp_ep_create(sender().worker(), &ep_params, &ep);
    }
    ucp_worker_release_address(receiver().worker(), address);

    /* The first lane was connected, the second one failed */
    EXPECT_EQ(UCS_ERR_NO_RESOURCE, status);
    EXPECT_EQ(1u, m_num_created);
    EXPECT_EQ(m_num_created, m_num_destroyed);
}

UCP_INSTANTIATE_TEST_CASE_TLS(test_ucp_ep_connect_fail, shm, "shm")
