#include "envoy/config/filter/network/http_connection_manager/v2/http_connection_manager.pb.validate.h"

#include "common/buffer/buffer_impl.h"
#include "common/config/filter_json.h"
#include "common/http/date_provider_impl.h"

#include "extensions/filters/network/http_connection_manager/config.h"

#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/test_base.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"

using testing::_;
using testing::ContainerEq;
using testing::Return;

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace HttpConnectionManager {

envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager
parseHttpConnectionManagerFromJson(const std::string& json_string) {
  envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager
      http_connection_manager;
  auto json_object_ptr = Json::Factory::loadFromString(json_string);
  NiceMock<Stats::MockStore> scope;
  Config::FilterJson::translateHttpConnectionManager(*json_object_ptr, http_connection_manager,
                                                     scope.statsOptions());
  return http_connection_manager;
}

envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager
parseHttpConnectionManagerFromV2Yaml(const std::string& yaml) {
  envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager
      http_connection_manager;
  MessageUtil::loadFromYaml(yaml, http_connection_manager);
  return http_connection_manager;
}

class HttpConnectionManagerConfigTest : public TestBase {
public:
  NiceMock<Server::Configuration::MockFactoryContext> context_;
  Http::SlowDateProviderImpl date_provider_{context_.dispatcher().timeSource()};
  NiceMock<Router::MockRouteConfigProviderManager> route_config_provider_manager_;
};

TEST_F(HttpConnectionManagerConfigTest, ValidateFail) {
  EXPECT_THROW(
      HttpConnectionManagerFilterConfigFactory().createFilterFactoryFromProto(
          envoy::config::filter::network::http_connection_manager::v2::HttpConnectionManager(),
          context_),
      ProtoValidationException);
}

TEST_F(HttpConnectionManagerConfigTest, InvalidFilterName) {
  const std::string json_string = R"EOF(
  {
    "codec_type": "http1",
    "stat_prefix": "router",
    "route_config":
    {
      "virtual_hosts": [
        {
          "name": "service",
          "domains": [ "*" ],
          "routes": [
            {
              "prefix": "/",
              "cluster": "cluster"
            }
          ]
        }
      ]
    },
    "filters": [
      { "name": "foo", "config": {} }
    ]
  }
  )EOF";

  EXPECT_THROW_WITH_MESSAGE(
      HttpConnectionManagerConfig(parseHttpConnectionManagerFromJson(json_string), context_,
                                  date_provider_, route_config_provider_manager_),
      EnvoyException, "Didn't find a registered implementation for name: 'foo'");
}

TEST_F(HttpConnectionManagerConfigTest, MiscConfig) {
  const std::string json_string = R"EOF(
  {
    "codec_type": "http1",
    "server_name": "foo",
    "stat_prefix": "router",
    "route_config":
    {
      "virtual_hosts": [
        {
          "name": "service",
          "domains": [ "*" ],
          "routes": [
            {
              "prefix": "/",
              "cluster": "cluster"
            }
          ]
        }
      ]
    },
    "tracing": {
      "operation_name": "ingress",
      "request_headers_for_tags": [ "foo" ]
    },
    "filters": [
      { "name": "http_dynamo_filter", "config": {} }
    ]
  }
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromJson(json_string), context_,
                                     date_provider_, route_config_provider_manager_);

  EXPECT_THAT(std::vector<Http::LowerCaseString>({Http::LowerCaseString("foo")}),
              ContainerEq(config.tracingConfig()->request_headers_for_tags_));
  EXPECT_EQ(*context_.local_info_.address_, config.localAddress());
  EXPECT_EQ("foo", config.serverName());
  EXPECT_EQ(5 * 60 * 1000, config.streamIdleTimeout().count());
}

TEST_F(HttpConnectionManagerConfigTest, UnixSocketInternalAddress) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  internal_address_config:
    unix_sockets: true
  route_config:
    name: local_route
  http_filters:
  - name: envoy.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_);
  Network::Address::PipeInstance unixAddress{"/foo"};
  Network::Address::Ipv4Instance internalIpAddress{"127.0.0.1", 0};
  Network::Address::Ipv4Instance externalIpAddress{"12.0.0.1", 0};
  EXPECT_TRUE(config.internalAddressConfig().isInternalAddress(unixAddress));
  EXPECT_TRUE(config.internalAddressConfig().isInternalAddress(internalIpAddress));
  EXPECT_FALSE(config.internalAddressConfig().isInternalAddress(externalIpAddress));
}

TEST_F(HttpConnectionManagerConfigTest, MaxRequestHeadersSizeDefault) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  http_filters:
  - name: envoy.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_);
  EXPECT_EQ(60, config.maxRequestHeadersKb());
}

TEST_F(HttpConnectionManagerConfigTest, MaxRequestHeadersSizeConfigured) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  max_request_headers_kb: 16
  route_config:
    name: local_route
  http_filters:
  - name: envoy.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_);
  EXPECT_EQ(16, config.maxRequestHeadersKb());
}

// Validated that an explicit zero stream idle timeout disables.
TEST_F(HttpConnectionManagerConfigTest, DisabledStreamIdleTimeout) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  stream_idle_timeout: 0s
  route_config:
    name: local_route
  http_filters:
  - name: envoy.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_);
  EXPECT_EQ(0, config.streamIdleTimeout().count());
}

TEST_F(HttpConnectionManagerConfigTest, ConfiguredRequestTimeout) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  request_timeout: 53s
  route_config:
    name: local_route
  http_filters:
  - name: envoy.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_);
  EXPECT_EQ(53 * 1000, config.requestTimeout().count());
}

TEST_F(HttpConnectionManagerConfigTest, DisabledRequestTimeout) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  request_timeout: 0s
  route_config:
    name: local_route
  http_filters:
  - name: envoy.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_);
  EXPECT_EQ(0, config.requestTimeout().count());
}

TEST_F(HttpConnectionManagerConfigTest, UnconfiguredRequestTimeout) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  http_filters:
  - name: envoy.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_);
  EXPECT_EQ(0, config.requestTimeout().count());
}

TEST_F(HttpConnectionManagerConfigTest, SingleDateProvider) {
  const std::string json_string = R"EOF(
  {
    "codec_type": "http1",
    "stat_prefix": "router",
    "route_config":
    {
      "virtual_hosts": [
        {
          "name": "service",
          "domains": [ "*" ],
          "routes": [
            {
              "prefix": "/",
              "cluster": "cluster"
            }
          ]
        }
      ]
    },
    "filters": [
      { "name": "http_dynamo_filter", "config": {} }
    ]
  }
  )EOF";

  Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json_string);
  HttpConnectionManagerFilterConfigFactory factory;
  // We expect a single slot allocation vs. multiple.
  EXPECT_CALL(context_.thread_local_, allocateSlot());
  Network::FilterFactoryCb cb1 = factory.createFilterFactory(*json_config, context_);
  Network::FilterFactoryCb cb2 = factory.createFilterFactory(*json_config, context_);
}

TEST_F(HttpConnectionManagerConfigTest, BadHttpConnectionMangerConfig) {
  std::string json_string = R"EOF(
  {
    "codec_type" : "http1",
    "stat_prefix" : "my_stat_prefix",
    "route_config" : {
      "virtual_hosts" : [
        {
          "name" : "default",
          "domains" : ["*"],
          "routes" : [
            {
              "prefix" : "/",
              "cluster": "fake_cluster"
            }
          ]
        }
      ]
    },
    "filter" : [{}]
  }
  )EOF";

  Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json_string);
  HttpConnectionManagerFilterConfigFactory factory;
  EXPECT_THROW(factory.createFilterFactory(*json_config, context_), Json::Exception);
}

TEST_F(HttpConnectionManagerConfigTest, BadAccessLogConfig) {
  std::string json_string = R"EOF(
  {
    "codec_type" : "http1",
    "stat_prefix" : "my_stat_prefix",
    "route_config" : {
      "virtual_hosts" : [
        {
          "name" : "default",
          "domains" : ["*"],
          "routes" : [
            {
              "prefix" : "/",
              "cluster": "fake_cluster"
            }
          ]
        }
      ]
    },
    "filters" : [
      {
        "type" : "both",
        "name" : "http_dynamo_filter",
        "config" : {}
      }
    ],
    "access_log" :[
      {
        "path" : "mypath",
        "filter" : []
      }
    ]
  }
  )EOF";

  Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json_string);
  HttpConnectionManagerFilterConfigFactory factory;
  EXPECT_THROW(factory.createFilterFactory(*json_config, context_), Json::Exception);
}

TEST_F(HttpConnectionManagerConfigTest, BadAccessLogType) {
  std::string json_string = R"EOF(
  {
    "codec_type" : "http1",
    "stat_prefix" : "my_stat_prefix",
    "route_config" : {
      "virtual_hosts" : [
        {
          "name" : "default",
          "domains" : ["*"],
          "routes" : [
            {
              "prefix" : "/",
              "cluster": "fake_cluster"
            }
          ]
        }
      ]
    },
    "filters" : [
      {
        "type" : "both",
        "name" : "http_dynamo_filter",
        "config" : {}
      }
    ],
    "access_log" :[
      {
        "path" : "mypath",
        "filter" : {
          "type" : "bad_type"
        }
      }
    ]
  }
  )EOF";

  Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json_string);
  HttpConnectionManagerFilterConfigFactory factory;
  EXPECT_THROW(factory.createFilterFactory(*json_config, context_), Json::Exception);
}

TEST_F(HttpConnectionManagerConfigTest, BadAccessLogNestedTypes) {
  std::string json_string = R"EOF(
  {
    "codec_type" : "http1",
    "stat_prefix" : "my_stat_prefix",
    "route_config" : {
      "virtual_hosts" : [
        {
          "name" : "default",
          "domains" : ["*"],
          "routes" : [
            {
              "prefix" : "/",
              "cluster": "fake_cluster"
            }
          ]
        }
      ]
    },
    "filters" : [
      {
        "type" : "both",
        "name" : "http_dynamo_filter",
        "config" : {}
      }
    ],
    "access_log" :[
      {
        "path": "/dev/null",
        "filter": {
          "type": "logical_and",
          "filters": [
            {
              "type": "logical_or",
              "filters": [
                {"type": "duration", "op": ">=", "value": 10000},
                {"type": "bad_type"}
              ]
            },
            {"type": "not_healthcheck"}
          ]
        }
      }
    ]
  }
  )EOF";

  Json::ObjectSharedPtr json_config = Json::Factory::loadFromString(json_string);
  HttpConnectionManagerFilterConfigFactory factory;
  EXPECT_THROW(factory.createFilterFactory(*json_config, context_), Json::Exception);
}

class FilterChainTest : public HttpConnectionManagerConfigTest {
public:
  const std::string basic_config_ = R"EOF(
  {
    "codec_type": "http1",
    "server_name": "foo",
    "stat_prefix": "router",
    "route_config":
    {
      "virtual_hosts": [
        {
          "name": "service",
          "domains": [ "*" ],
          "routes": [
            {
              "prefix": "/",
              "cluster": "cluster"
            }
          ]
        }
      ]
    },
    "filters": [
      { "name": "http_dynamo_filter", "config": {} },
      { "name": "router", "config": {} }
    ]
  }
  )EOF";
};

TEST_F(FilterChainTest, createFilterChain) {
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromJson(basic_config_), context_,
                                     date_provider_, route_config_provider_manager_);

  Http::MockFilterChainFactoryCallbacks callbacks;
  EXPECT_CALL(callbacks, addStreamFilter(_));        // Dynamo
  EXPECT_CALL(callbacks, addStreamDecoderFilter(_)); // Router
  config.createFilterChain(callbacks);
}

// Tests where upgrades are configured on via the HCM.
TEST_F(FilterChainTest, createUpgradeFilterChain) {
  auto hcm_config = parseHttpConnectionManagerFromJson(basic_config_);
  hcm_config.add_upgrade_configs()->set_upgrade_type("websocket");

  HttpConnectionManagerConfig config(hcm_config, context_, date_provider_,
                                     route_config_provider_manager_);

  NiceMock<Http::MockFilterChainFactoryCallbacks> callbacks;
  // Check the case where WebSockets are configured in the HCM, and no router
  // config is present. We should create an upgrade filter chain for
  // WebSockets.
  {
    EXPECT_CALL(callbacks, addStreamFilter(_));        // Dynamo
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)); // Router
    EXPECT_TRUE(config.createUpgradeFilterChain("WEBSOCKET", nullptr, callbacks));
  }

  // Check the case where WebSockets are configured in the HCM, and no router
  // config is present. We should not create an upgrade filter chain for Foo
  {
    EXPECT_CALL(callbacks, addStreamFilter(_)).Times(0);
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)).Times(0);
    EXPECT_FALSE(config.createUpgradeFilterChain("foo", nullptr, callbacks));
  }

  // Now override the HCM with a route-specific disabling of WebSocket to
  // verify route-specific disabling works.
  {
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("WebSocket", false));
    EXPECT_FALSE(config.createUpgradeFilterChain("WEBSOCKET", &upgrade_map, callbacks));
  }

  // For paranoia's sake make sure route-specific enabling doesn't break
  // anything.
  {
    EXPECT_CALL(callbacks, addStreamFilter(_));        // Dynamo
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)); // Router
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("WebSocket", true));
    EXPECT_TRUE(config.createUpgradeFilterChain("WEBSOCKET", &upgrade_map, callbacks));
  }
}

// Tests where upgrades are configured off via the HCM.
TEST_F(FilterChainTest, createUpgradeFilterChainHCMDisabled) {
  auto hcm_config = parseHttpConnectionManagerFromJson(basic_config_);
  hcm_config.add_upgrade_configs()->set_upgrade_type("websocket");
  hcm_config.mutable_upgrade_configs(0)->mutable_enabled()->set_value(false);

  HttpConnectionManagerConfig config(hcm_config, context_, date_provider_,
                                     route_config_provider_manager_);

  NiceMock<Http::MockFilterChainFactoryCallbacks> callbacks;
  // Check the case where WebSockets are off in the HCM, and no router config is present.
  { EXPECT_FALSE(config.createUpgradeFilterChain("WEBSOCKET", nullptr, callbacks)); }

  // Check the case where WebSockets are off in the HCM and in router config.
  {
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("WebSocket", false));
    EXPECT_FALSE(config.createUpgradeFilterChain("WEBSOCKET", &upgrade_map, callbacks));
  }

  // With a route-specific enabling for WebSocket, WebSocket should work.
  {
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("WebSocket", true));
    EXPECT_TRUE(config.createUpgradeFilterChain("WEBSOCKET", &upgrade_map, callbacks));
  }

  // With only a route-config we should do what the route config says.
  {
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("foo", true));
    upgrade_map.emplace(std::make_pair("bar", false));
    EXPECT_TRUE(config.createUpgradeFilterChain("foo", &upgrade_map, callbacks));
    EXPECT_FALSE(config.createUpgradeFilterChain("bar", &upgrade_map, callbacks));
    EXPECT_FALSE(config.createUpgradeFilterChain("eep", &upgrade_map, callbacks));
  }
}

TEST_F(FilterChainTest, createCustomUpgradeFilterChain) {
  auto hcm_config = parseHttpConnectionManagerFromJson(basic_config_);
  auto websocket_config = hcm_config.add_upgrade_configs();
  websocket_config->set_upgrade_type("websocket");

  ASSERT_TRUE(websocket_config->add_filters()->ParseFromString("\n\fenvoy.router"));

  auto foo_config = hcm_config.add_upgrade_configs();
  foo_config->set_upgrade_type("foo");
  foo_config->add_filters()->ParseFromString("\n\fenvoy.router");
  foo_config->add_filters()->ParseFromString("\n"
                                             "\x18"
                                             "envoy.http_dynamo_filter");
  foo_config->add_filters()->ParseFromString("\n"
                                             "\x18"
                                             "envoy.http_dynamo_filter");

  HttpConnectionManagerConfig config(hcm_config, context_, date_provider_,
                                     route_config_provider_manager_);

  {
    Http::MockFilterChainFactoryCallbacks callbacks;
    EXPECT_CALL(callbacks, addStreamFilter(_));        // Dynamo
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)); // Router
    config.createFilterChain(callbacks);
  }

  {
    Http::MockFilterChainFactoryCallbacks callbacks;
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)); // Router
    EXPECT_TRUE(config.createUpgradeFilterChain("websocket", nullptr, callbacks));
  }

  {
    Http::MockFilterChainFactoryCallbacks callbacks;
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_));   // Router
    EXPECT_CALL(callbacks, addStreamFilter(_)).Times(2); // Dynamo
    EXPECT_TRUE(config.createUpgradeFilterChain("Foo", nullptr, callbacks));
  }
}

TEST_F(FilterChainTest, invalidConfig) {
  auto hcm_config = parseHttpConnectionManagerFromJson(basic_config_);
  hcm_config.add_upgrade_configs()->set_upgrade_type("WEBSOCKET");
  hcm_config.add_upgrade_configs()->set_upgrade_type("websocket");

  EXPECT_THROW_WITH_MESSAGE(HttpConnectionManagerConfig(hcm_config, context_, date_provider_,
                                                        route_config_provider_manager_),
                            EnvoyException,
                            "Error: multiple upgrade configs with the same name: 'websocket'");
}

} // namespace HttpConnectionManager
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
