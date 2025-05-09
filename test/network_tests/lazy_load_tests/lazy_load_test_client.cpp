// Copyright (C) 2023 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <iomanip>

#include "lazy_load_test_client.hpp"

lazy_load_test_client::lazy_load_test_client()
    : app_(vsomeip::runtime::get()->create_application()),
      current_service_availability_status_(false),
      received_responses_(0),
      received_allowed_events_(0) {}

bool lazy_load_test_client::init() {
    if (!app_->init()) {
        ADD_FAILURE() << "Couldn't initialize application";
        return false;
    }

    app_->register_state_handler(
            std::bind(&lazy_load_test_client::on_state, this,
                    std::placeholders::_1));

    app_->register_message_handler(vsomeip::ANY_SERVICE,
            vsomeip_test::TEST_SERVICE_INSTANCE_ID, vsomeip::ANY_METHOD,
            std::bind(&lazy_load_test_client::on_message, this,
                    std::placeholders::_1));

    app_->register_message_handler(vsomeip::ANY_SERVICE,
            TEST_INSTANCE_LAZY, vsomeip::ANY_METHOD,
            std::bind(&lazy_load_test_client::on_message, this,
                    std::placeholders::_1));

    app_->register_availability_handler(vsomeip_test::TEST_SERVICE_SERVICE_ID,
            vsomeip_test::TEST_SERVICE_INSTANCE_ID,
            std::bind(&lazy_load_test_client::on_availability, this,
                    std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3));

    app_->register_availability_handler(SERVICE_TO_BE_REFUSED,
            vsomeip_test::TEST_SERVICE_INSTANCE_ID,
            std::bind(&lazy_load_test_client::on_availability, this,
                    std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3));

    app_->register_availability_handler(vsomeip_test::TEST_SERVICE_SERVICE_ID,
            TEST_INSTANCE_LAZY,
            std::bind(&lazy_load_test_client::on_availability, this,
                    std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3));

    sender_ = std::thread(std::bind(&lazy_load_test_client::run, this));

    return true;
}

void lazy_load_test_client::start() {
    VSOMEIP_INFO << "Starting...";

    app_->start();
}

void lazy_load_test_client::stop() {
    VSOMEIP_INFO << "Stopping...";

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    app_->clear_all_handler();
    app_->stop();
}

void lazy_load_test_client::on_state(vsomeip::state_type_e _state) {
    VSOMEIP_INFO << "CLIENT ON_STATE...";
    if (_state == vsomeip::state_type_e::ST_REGISTERED) {
        app_->request_service(vsomeip_test::TEST_SERVICE_SERVICE_ID,
                vsomeip_test::TEST_SERVICE_INSTANCE_ID, false);

        // request not allowed service ID
        app_->request_service(SERVICE_TO_BE_REFUSED,
                vsomeip_test::TEST_SERVICE_INSTANCE_ID, false);

        // request not allowed instance ID
        app_->request_service(vsomeip_test::TEST_SERVICE_SERVICE_ID,
                TEST_INSTANCE_LAZY, false);

        // request events of eventgroup 0x01 which holds events 0x8001 (allowed) and 0x8002 (denied)
        std::set<vsomeip::eventgroup_t> its_eventgroups;
        its_eventgroups.insert(EVENT_GROUP);

        app_->request_event(vsomeip_test::TEST_SERVICE_SERVICE_ID, vsomeip_test::TEST_SERVICE_INSTANCE_ID,
                static_cast<vsomeip::event_t>(EVENT_TO_ACCEPT),
                its_eventgroups, vsomeip::event_type_e::ET_FIELD,
                vsomeip::reliability_type_e::RT_UNRELIABLE);
        app_->request_event(vsomeip_test::TEST_SERVICE_SERVICE_ID, vsomeip_test::TEST_SERVICE_INSTANCE_ID,
                static_cast<vsomeip::event_t>(EVENT_TO_REFUSE),
                its_eventgroups, vsomeip::event_type_e::ET_FIELD,
                vsomeip::reliability_type_e::RT_UNRELIABLE);

        app_->subscribe(vsomeip_test::TEST_SERVICE_SERVICE_ID, vsomeip_test::TEST_SERVICE_INSTANCE_ID, EVENT_GROUP,
                vsomeip::DEFAULT_MAJOR, static_cast<vsomeip::event_t>(EVENT_TO_ACCEPT));

        app_->subscribe(vsomeip_test::TEST_SERVICE_SERVICE_ID, vsomeip_test::TEST_SERVICE_INSTANCE_ID, EVENT_GROUP,
            vsomeip::DEFAULT_MAJOR, static_cast<vsomeip::event_t>(EVENT_TO_REFUSE));

    }
}

void lazy_load_test_client::on_availability(vsomeip::service_t _service,
        vsomeip::instance_t _instance, bool _is_service_available) {

    VSOMEIP_INFO << "CLIENT ON_AVAILABILITY...";
    VSOMEIP_INFO << std::hex << "Client 0x" << app_->get_client() << " : Service [" << std::hex
                 << std::setfill('0') << std::setw(4) << _service << '.' << _instance << "] is "
                 << (_is_service_available ? "available." : "NOT available.");

    // Check that only the allowed service is available
    if (_is_service_available) {
        EXPECT_EQ(vsomeip_test::TEST_SERVICE_SERVICE_ID, _service)
            << "Unexpected Service ID on_availability";
        EXPECT_EQ(vsomeip_test::TEST_SERVICE_INSTANCE_ID, _instance)
            << "Unexpected Instance ID on_availability";
    }
    if (vsomeip_test::TEST_SERVICE_SERVICE_ID == _service
            && vsomeip_test::TEST_SERVICE_INSTANCE_ID == _instance) {
        std::unique_lock<std::mutex> its_lock(mutex_);
        if (current_service_availability_status_ && !_is_service_available) {
            current_service_availability_status_ = false;
        } else if (_is_service_available && !current_service_availability_status_) {
            current_service_availability_status_ = true;
            condition_.notify_one();
        }
    }
}

void lazy_load_test_client::on_message(const std::shared_ptr<vsomeip::message> &_response) {
    VSOMEIP_INFO << "Received a response from Service ["
                 << std::hex << std::setfill('0') << std::setw(4) << _response->get_service()
                 << '.' << std::setw(4) << _response->get_instance()
                 << "] to Client/Session [" << std::setw(4) << _response->get_client()
                 << '/' << std::setw(4) << _response->get_session() << ']';

    if (_response->get_message_type() == vsomeip::message_type_e::MT_RESPONSE) {
        EXPECT_EQ(vsomeip_test::TEST_SERVICE_SERVICE_ID,  _response->get_service())
            << "Unexpected Service ID on_message response";
        EXPECT_EQ(vsomeip_test::TEST_SERVICE_INSTANCE_ID, _response->get_instance())
            << "Unexpected Instance ID on_message response";
        EXPECT_EQ(vsomeip_test::TEST_SERVICE_METHOD_ID, _response->get_method())
            << "Unexpected Method ID on_message response";


        if (_response->get_service() == vsomeip_test::TEST_SERVICE_SERVICE_ID &&
                _response->get_instance() == vsomeip_test::TEST_SERVICE_INSTANCE_ID &&
                _response->get_method() == vsomeip_test::TEST_SERVICE_METHOD_ID) {
            received_responses_++;
            if (received_responses_ == NUMBER_OF_MESSAGES_TO_SEND) {
                VSOMEIP_WARNING << std::hex << app_->get_client()
                        << ": Received all messages ~> going down!";
            }
        }

    } else if (_response->get_message_type() == vsomeip::message_type_e::MT_NOTIFICATION) {

        // check that only allowed event 0x8001 is received for client-sample-a
        EXPECT_EQ(vsomeip_test::TEST_SERVICE_SERVICE_ID,  _response->get_service())
            << "Unexpected Service ID on_message notification";
        EXPECT_EQ(vsomeip_test::TEST_SERVICE_INSTANCE_ID, _response->get_instance())
            << "Unexpected Instance ID on_message notification";
        EXPECT_EQ(EVENT_TO_ACCEPT, _response->get_method())
            << "Unexpected Method ID on_message notification";

        received_allowed_events_++;
    }
}

void lazy_load_test_client::run() {
    for (std::uint32_t i = 0; i < NUMBER_OF_MESSAGES_TO_SEND; ++i) {
        {
            std::unique_lock<std::mutex> its_lock(mutex_);
            while (!current_service_availability_status_) {
                condition_.wait(its_lock);
            }
        }

        auto request = vsomeip::runtime::get()->create_request(false);

        request->set_service(vsomeip_test::TEST_SERVICE_SERVICE_ID);
        request->set_instance(vsomeip_test::TEST_SERVICE_INSTANCE_ID);
        request->set_method(vsomeip_test::TEST_SERVICE_METHOD_ID);

        // send a request which is allowed by policy -> expect answer
        app_->send(request);

        // send a request with a not allowed method ID -> expect no answer
        request->set_method(METHOD_TO_BE_REFUSED);
        app_->send(request);

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    EXPECT_EQ(NUMBER_OF_MESSAGES_TO_SEND, received_responses_)
            << "Unexpected received_responses_ run()";
    EXPECT_EQ(received_allowed_events_, static_cast<std::uint32_t>(EXPECTED_EVENTS))
            << "Unexpected received_allowed_events run()";

    stop();
}

void lazy_load_test_client::join_sender_thread() {
    if (sender_.joinable()) {
        sender_.join();
    }
}

TEST(someip_lazy_load_test, normal_client) {
    lazy_load_test_client test_client;
    if (test_client.init()) {
        test_client.start();
        test_client.join_sender_thread();
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
