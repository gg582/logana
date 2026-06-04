#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_ROWS 100
#define NUM_CLUSTERS 3

#ifndef SAFE_FREE
#define SAFE_FREE(ptr) do { free(ptr); ptr = NULL; } while(0)
#endif

typedef struct {
    char   timestamp[32];
    char   service[32];
    double latency_ms;
    double cpu_util;
    double mem_rss_mb;
    bool   valid;
    int    cluster_id;
} LogEntry;

static const char *RAW_LINES[NUM_ROWS] = {
    "{\"timestamp\":\"2026-05-31T18:00:01.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":20.0,\"http_status\":200,\"active_users\":1400,\"cpu_util\":40.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:02.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":110.3,\"http_status\":200,\"active_users\":451,\"cpu_util\":30.1,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:00:03.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":4.1,\"http_status\":200,\"active_users\":8820,\"cpu_util\":15.1,\"mem_rss_mb\":128.0}",
    "{\"timestamp\":\"2026-05-31T18:00:04.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":21.5,\"http_status\":200,\"active_users\":1406,\"cpu_util\":40.6,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:05.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":111.2,\"http_status\":200,\"active_users\":450,\"cpu_util\":30.4,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:00:06.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":4.25,\"http_status\":200,\"active_users\":8850,\"cpu_util\":15.25,\"mem_rss_mb\":128.0}",
    "{\"timestamp\":\"2026-05-31T18:00:07.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":23.0,\"http_status\":200,\"active_users\":1412,\"cpu_util\":41.2,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:08.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":112.1,\"http_status\":200,\"active_users\":452,\"cpu_util\":30.7,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:00:09.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":4.4,\"http_status\":200,\"active_users\":8880,\"cpu_util\":15.4,\"mem_rss_mb\":128.0}",
    "{\"timestamp\":\"2026-05-31T18:00:10.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":24.5,\"http_status\":200,\"active_users\":1418,\"cpu_util\":41.8,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:11.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":113.0,\"http_status\":200,\"active_users\":451,\"cpu_util\":31.0,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:00:12.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":4.55,\"http_status\":200,\"active_users\":8910,\"cpu_util\":15.55,\"mem_rss_mb\":128.0}",
    "{\"timestamp\":\"2026-05-31T18:00:13.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":26.0,\"http_status\":200,\"active_users\":1424,\"cpu_util\":42.4,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:14.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":113.9,\"http_status\":200,\"active_users\":453,\"cpu_util\":31.3,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:00:15.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":4.7,\"http_status\":200,\"active_users\":8940,\"cpu_util\":15.7,\"mem_rss_mb\":128.0}",
    "{\"timestamp\":\"2026-05-31T18:00:16.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":27.5,\"http_status\":200,\"active_users\":1430,\"cpu_util\":43.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:17.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":114.8,\"http_status\":200,\"active_users\":452,\"cpu_util\":31.6,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:00:18.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":4.85,\"http_status\":200,\"active_users\":8970,\"cpu_util\":15.85,\"mem_rss_mb\":128.0}",
    "{\"timestamp\":\"2026-05-31T18:00:19.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":29.0,\"http_status\":200,\"active_users\":1436,\"cpu_util\":43.6,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:20.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":115.7,\"http_status\":200,\"active_users\":454,\"cpu_util\":31.9,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:00:21.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":30.0,\"http_status\":200,\"active_users\":1500,\"cpu_util\":45.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:22.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":125.0,\"http_status\":200,\"active_users\":1650,\"cpu_util\":49.5,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:23.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":220.0,\"http_status\":200,\"active_users\":1800,\"cpu_util\":54.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:24.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":315.0,\"http_status\":200,\"active_users\":1950,\"cpu_util\":58.5,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:25.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":410.0,\"http_status\":200,\"active_users\":2100,\"cpu_util\":63.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:26.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":505.0,\"http_status\":200,\"active_users\":2250,\"cpu_util\":67.5,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:27.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":600.0,\"http_status\":200,\"active_users\":2400,\"cpu_util\":72.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:28.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":695.0,\"http_status\":200,\"active_users\":2550,\"cpu_util\":76.5,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:29.000Z\",\"level\":\"WARN\",\"service\":\"auth-api\",\"latency_ms\":790.0,\"http_status\":200,\"active_users\":2700,\"cpu_util\":81.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:30.000Z\",\"level\":\"WARN\",\"service\":\"auth-api\",\"latency_ms\":885.0,\"http_status\":200,\"active_users\":2850,\"cpu_util\":85.5,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:31.000Z\",\"level\":\"WARN\",\"service\":\"auth-api\",\"latency_ms\":980.0,\"http_status\":200,\"active_users\":3000,\"cpu_util\":90.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:32.000Z\",\"level\":\"WARN\",\"service\":\"auth-api\",\"latency_ms\":1075.0,\"http_status\":200,\"active_users\":3150,\"cpu_util\":94.5,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:33.000Z\",\"level\":\"WARN\",\"service\":\"auth-api\",\"latency_ms\":1170.0,\"http_status\":200,\"active_users\":3300,\"cpu_util\":99.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:34.000Z\",\"level\":\"WARN\",\"service\":\"auth-api\",\"latency_ms\":1265.0,\"http_status\":200,\"active_users\":3450,\"cpu_util\":99.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:35.000Z\",\"level\":\"ERROR\",\"service\":\"auth-api\",\"latency_ms\":1360.0,\"http_status\":500,\"active_users\":3600,\"cpu_util\":99.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:36.000Z\",\"level\":\"ERROR\",\"service\":\"auth-api\",\"latency_ms\":1455.0,\"http_status\":500,\"active_users\":3750,\"cpu_util\":99.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:37.000Z\",\"level\":\"ERROR\",\"service\":\"auth-api\",\"latency_ms\":1550.0,\"http_status\":500,\"active_users\":3900,\"cpu_util\":99.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:38.000Z\",\"level\":\"ERROR\",\"service\":\"auth-api\",\"latency_ms\":1645.0,\"http_status\":500,\"active_users\":4050,\"cpu_util\":99.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:39.000Z\",\"level\":\"ERROR\",\"service\":\"auth-api\",\"latency_ms\":1740.0,\"http_status\":500,\"active_users\":4200,\"cpu_util\":99.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:40.000Z\",\"level\":\"FATAL\",\"service\":\"auth-api\",\"latency_ms\":1835.0,\"http_status\":500,\"active_users\":4350,\"cpu_util\":99.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:00:41.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":120.0,\"http_status\":200,\"active_users\":460,\"cpu_util\":32.0,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:00:42.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":122.0,\"http_status\":200,\"active_users\":461,\"cpu_util\":32.5,\"mem_rss_mb\":1069.0}",
    "{\"timestamp\":\"2026-05-31T18:00:43.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":124.0,\"http_status\":200,\"active_users\":462,\"cpu_util\":33.0,\"mem_rss_mb\":1114.0}",
    "{\"timestamp\":\"2026-05-31T18:00:44.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":126.0,\"http_status\":200,\"active_users\":460,\"cpu_util\":33.5,\"mem_rss_mb\":1159.0}",
    "{\"timestamp\":\"2026-05-31T18:00:45.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":128.0,\"http_status\":200,\"active_users\":461,\"cpu_util\":34.0,\"mem_rss_mb\":1204.0}",
    "{\"timestamp\":\"2026-05-31T18:00:46.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":130.0,\"http_status\":200,\"active_users\":462,\"cpu_util\":34.5,\"mem_rss_mb\":1249.0}",
    "{\"timestamp\":\"2026-05-31T18:00:47.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":132.0,\"http_status\":200,\"active_users\":460,\"cpu_util\":35.0,\"mem_rss_mb\":1294.0}",
    "{\"timestamp\":\"2026-05-31T18:00:48.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":134.0,\"http_status\":200,\"active_users\":461,\"cpu_util\":35.5,\"mem_rss_mb\":1339.0}",
    "{\"timestamp\":\"2026-05-31T18:00:49.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":136.0,\"http_status\":200,\"active_users\":462,\"cpu_util\":36.0,\"mem_rss_mb\":1384.0}",
    "{\"timestamp\":\"2026-05-31T18:00:50.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":138.0,\"http_status\":200,\"active_users\":460,\"cpu_util\":36.5,\"mem_rss_mb\":1429.0}",
    "{\"timestamp\":\"2026-05-31T18:00:51.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":140.0,\"http_status\":200,\"active_users\":461,\"cpu_util\":37.0,\"mem_rss_mb\":1474.0}",
    "{\"timestamp\":\"2026-05-31T18:00:52.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":142.0,\"http_status\":200,\"active_users\":462,\"cpu_util\":37.5,\"mem_rss_mb\":1519.0}",
    "{\"timestamp\":\"2026-05-31T18:00:53.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":144.0,\"http_status\":200,\"active_users\":460,\"cpu_util\":38.0,\"mem_rss_mb\":1564.0}",
    "{\"timestamp\":\"2026-05-31T18:00:54.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":146.0,\"http_status\":200,\"active_users\":461,\"cpu_util\":38.5,\"mem_rss_mb\":1609.0}",
    "{\"timestamp\":\"2026-05-31T18:00:55.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":148.0,\"http_status\":200,\"active_users\":462,\"cpu_util\":39.0,\"mem_rss_mb\":1654.0}",
    "{\"timestamp\":\"2026-05-31T18:00:56.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":150.0,\"http_status\":200,\"active_users\":460,\"cpu_util\":39.5,\"mem_rss_mb\":1699.0}",
    "{\"timestamp\":\"2026-05-31T18:00:57.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":152.0,\"http_status\":200,\"active_users\":461,\"cpu_util\":40.0,\"mem_rss_mb\":1744.0}",
    "{\"timestamp\":\"2026-05-31T18:00:58.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":154.0,\"http_status\":200,\"active_users\":462,\"cpu_util\":40.5,\"mem_rss_mb\":1789.0}",
    "{\"timestamp\":\"2026-05-31T18:00:59.000Z\",\"level\":\"WARN\",\"service\":\"payment-v2\",\"latency_ms\":156.0,\"http_status\":200,\"active_users\":460,\"cpu_util\":41.0,\"mem_rss_mb\":1834.0}",
    "{\"timestamp\":\"2026-05-31T18:01:00.000Z\",\"level\":\"WARN\",\"service\":\"payment-v2\",\"latency_ms\":158.0,\"http_status\":200,\"active_users\":461,\"cpu_util\":41.5,\"mem_rss_mb\":1879.0}",
    "{\"timestamp\":\"2026-05-31T18:01:01.000Z\",\"level\":\"WARN\",\"service\":\"payment-v2\",\"latency_ms\":160.0,\"http_status\":200,\"active_users\":462,\"cpu_util\":42.0,\"mem_rss_mb\":1924.0}",
    "{\"timestamp\":\"2026-05-31T18:01:02.000Z\",\"level\":\"WARN\",\"service\":\"payment-v2\",\"latency_ms\":162.0,\"http_status\":200,\"active_users\":460,\"cpu_util\":42.5,\"mem_rss_mb\":1969.0}",
    "{\"timestamp\":\"2026-05-31T18:01:03.000Z\",\"level\":\"WARN\",\"service\":\"payment-v2\",\"latency_ms\":164.0,\"http_status\":200,\"active_users\":461,\"cpu_util\":43.0,\"mem_rss_mb\":2014.0}",
    "{\"timestamp\":\"2026-05-31T18:01:04.000Z\",\"level\":\"WARN\",\"service\":\"payment-v2\",\"latency_ms\":166.0,\"http_status\":200,\"active_users\":462,\"cpu_util\":43.5,\"mem_rss_mb\":2059.0}",
    "{\"timestamp\":\"2026-05-31T18:01:05.000Z\",\"level\":\"WARN\",\"service\":\"payment-v2\",\"latency_ms\":168.0,\"http_status\":200,\"active_users\":460,\"cpu_util\":44.0,\"mem_rss_mb\":2104.0}",
    "{\"timestamp\":\"2026-05-31T18:01:06.000Z\",\"level\":\"ERROR\",\"service\":\"payment-v2\",\"latency_ms\":0.0,\"http_status\":503,\"active_users\":0,\"cpu_util\":2.0,\"mem_rss_mb\":2149.0}",
    "{\"timestamp\":\"2026-05-31T18:01:07.000Z\",\"level\":\"ERROR\",\"service\":\"payment-v2\",\"latency_ms\":0.0,\"http_status\":503,\"active_users\":0,\"cpu_util\":2.0,\"mem_rss_mb\":2194.0}",
    "{\"timestamp\":\"2026-05-31T18:01:08.000Z\",\"level\":\"ERROR\",\"service\":\"payment-v2\",\"latency_ms\":0.0,\"http_status\":503,\"active_users\":0,\"cpu_util\":2.0,\"mem_rss_mb\":2239.0}",
    "{\"timestamp\":\"2026-05-31T18:01:09.000Z\",\"level\":\"ERROR\",\"service\":\"payment-v2\",\"latency_ms\":0.0,\"http_status\":503,\"active_users\":0,\"cpu_util\":2.0,\"mem_rss_mb\":2284.0}",
    "{\"timestamp\":\"2026-05-31T18:01:10.000Z\",\"level\":\"ERROR\",\"service\":\"payment-v2\",\"latency_ms\":0.0,\"http_status\":503,\"active_users\":0,\"cpu_util\":2.0,\"mem_rss_mb\":2329.0}",
    "{\"timestamp\":\"2026-05-31T18:01:11.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":5.0,\"http_status\":200,\"active_users\":9000,\"cpu_util\":20.0,\"mem_rss_mb\":128.0}",
    "{\"timestamp\":\"2026-05-31T18:01:12.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":10.0,\"http_status\":200,\"active_users\":9400,\"cpu_util\":24.5,\"mem_rss_mb\":129.0}",
    "{\"timestamp\":\"2026-05-31T18:01:13.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":15.0,\"http_status\":200,\"active_users\":9800,\"cpu_util\":29.0,\"mem_rss_mb\":130.0}",
    "{\"timestamp\":\"2026-05-31T18:01:14.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":20.0,\"http_status\":200,\"active_users\":10200,\"cpu_util\":33.5,\"mem_rss_mb\":131.0}",
    "{\"timestamp\":\"2026-05-31T18:01:15.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":25.0,\"http_status\":200,\"active_users\":10600,\"cpu_util\":38.0,\"mem_rss_mb\":132.0}",
    "{\"timestamp\":\"2026-05-31T18:01:16.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":30.0,\"http_status\":200,\"active_users\":11000,\"cpu_util\":42.5,\"mem_rss_mb\":133.0}",
    "{\"timestamp\":\"2026-05-31T18:01:17.000Z\",\"level\":\"INFO\",\"service\":\"mesh-router\",\"latency_ms\":35.0,\"http_status\":200,\"active_users\":11400,\"cpu_util\":47.0,\"mem_rss_mb\":134.0}",
    "{\"timestamp\":\"2026-05-31T18:01:18.000Z\",\"level\":\"WARN\",\"service\":\"mesh-router\",\"latency_ms\":40.0,\"http_status\":200,\"active_users\":11800,\"cpu_util\":51.5,\"mem_rss_mb\":135.0}",
    "{\"timestamp\":\"2026-05-31T18:01:19.000Z\",\"level\":\"WARN\",\"service\":\"mesh-router\",\"latency_ms\":45.0,\"http_status\":200,\"active_users\":12200,\"cpu_util\":56.0,\"mem_rss_mb\":136.0}",
    "{\"timestamp\":\"2026-05-31T18:01:20.000Z\",\"level\":\"WARN\",\"service\":\"mesh-router\",\"latency_ms\":50.0,\"http_status\":200,\"active_users\":12600,\"cpu_util\":60.5,\"mem_rss_mb\":137.0}",
    "{\"timestamp\":\"2026-05-31T18:01:21.000Z\",\"level\":\"WARN\",\"service\":\"mesh-router\",\"latency_ms\":55.0,\"http_status\":200,\"active_users\":13000,\"cpu_util\":65.0,\"mem_rss_mb\":138.0}",
    "{\"timestamp\":\"2026-05-31T18:01:22.000Z\",\"level\":\"WARN\",\"service\":\"mesh-router\",\"latency_ms\":60.0,\"http_status\":200,\"active_users\":13400,\"cpu_util\":69.5,\"mem_rss_mb\":139.0}",
    "{\"timestamp\":\"2026-05-31T18:01:23.000Z\",\"level\":\"WARN\",\"service\":\"mesh-router\",\"latency_ms\":65.0,\"http_status\":200,\"active_users\":13800,\"cpu_util\":74.0,\"mem_rss_mb\":140.0}",
    "{\"timestamp\":\"2026-05-31T18:01:24.000Z\",\"level\":\"WARN\",\"service\":\"mesh-router\",\"latency_ms\":70.0,\"http_status\":200,\"active_users\":14200,\"cpu_util\":78.5,\"mem_rss_mb\":141.0}",
    "{\"timestamp\":\"2026-05-31T18:01:25.000Z\",\"level\":\"FATAL\",\"service\":\"mesh-router\",\"latency_ms\":0.0,\"http_status\":504,\"active_users\":0,\"cpu_util\":83.0,\"mem_rss_mb\":142.0}",
    "{\"timestamp\":\"2026-05-31T18:01:26.000Z\",\"level\":\"FATAL\",\"service\":\"mesh-router\",\"latency_ms\":0.0,\"http_status\":504,\"active_users\":0,\"cpu_util\":87.5,\"mem_rss_mb\":143.0}",
    "{\"timestamp\":\"2026-05-31T18:01:27.000Z\",\"level\":\"FATAL\",\"service\":\"mesh-router\",\"latency_ms\":0.0,\"http_status\":504,\"active_users\":0,\"cpu_util\":92.0,\"mem_rss_mb\":144.0}",
    "{\"timestamp\":\"2026-05-31T18:01:28.000Z\",\"level\":\"FATAL\",\"service\":\"mesh-router\",\"latency_ms\":0.0,\"http_status\":504,\"active_users\":0,\"cpu_util\":96.5,\"mem_rss_mb\":145.0}",
    "{\"timestamp\":\"2026-05-31T18:01:29.000Z\",\"level\":\"FATAL\",\"service\":\"mesh-router\",\"latency_ms\":0.0,\"http_status\":504,\"active_users\":0,\"cpu_util\":99.0,\"mem_rss_mb\":146.0}",
    "{\"timestamp\":\"2026-05-31T18:01:30.000Z\",\"level\":\"FATAL\",\"service\":\"mesh-router\",\"latency_ms\":0.0,\"http_status\":504,\"active_users\":0,\"cpu_util\":99.0,\"mem_rss_mb\":147.0}",
    "{\"timestamp\":\"2026-05-31T18:01:31.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":25.0,\"http_status\":200,\"active_users\":1200,\"cpu_util\":35.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:01:32.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":115.0,\"http_status\":200,\"active_users\":400,\"cpu_util\":25.0,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:01:33.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":25.0,\"http_status\":200,\"active_users\":1200,\"cpu_util\":35.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:01:34.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":115.0,\"http_status\":200,\"active_users\":400,\"cpu_util\":25.0,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:01:35.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":25.0,\"http_status\":200,\"active_users\":1200,\"cpu_util\":35.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:01:36.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":115.0,\"http_status\":200,\"active_users\":400,\"cpu_util\":25.0,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:01:37.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":25.0,\"http_status\":200,\"active_users\":1200,\"cpu_util\":35.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:01:38.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":115.0,\"http_status\":200,\"active_users\":400,\"cpu_util\":25.0,\"mem_rss_mb\":1024.0}",
    "{\"timestamp\":\"2026-05-31T18:01:39.000Z\",\"level\":\"INFO\",\"service\":\"auth-api\",\"latency_ms\":25.0,\"http_status\":200,\"active_users\":1200,\"cpu_util\":35.0,\"mem_rss_mb\":512.0}",
    "{\"timestamp\":\"2026-05-31T18:01:40.000Z\",\"level\":\"INFO\",\"service\":\"payment-v2\",\"latency_ms\":115.0,\"http_status\":200,\"active_users\":400,\"cpu_util\":25.0,\"mem_rss_mb\":1024.0}"
};

/* --------------------------------------------------------------------------
 * ISO 8601 parser
 * -------------------------------------------------------------------------- */
double parse_iso8601_to_seconds(const char *timestamp_str)
{
    if (!timestamp_str || timestamp_str[0] == '\0')
        return 0.0;

    const char *s = timestamp_str;
    size_t len = strlen(s);
    if (len < 19)
        return 0.0;

    int year   = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int month  = (s[5] - '0') * 10 + (s[6] - '0');
    int day    = (s[8] - '0') * 10 + (s[9] - '0');
    int hour   = (s[11] - '0') * 10 + (s[12] - '0');
    int minute = (s[14] - '0') * 10 + (s[15] - '0');
    int second = (s[17] - '0') * 10 + (s[18] - '0');

    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':')
        return 0.0;

    int ms = 0;
    if (len >= 23 && s[19] == '.') {
        ms = (s[20] - '0') * 100 + (s[21] - '0') * 10 + (s[22] - '0');
    }

    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 || ms < 0 || ms > 999) {
        return 0.0;
    }

    static const int days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    int64_t y = (int64_t)year - 1;
    int64_t days = y * 365 + y / 4 - y / 100 + y / 400;
    days += days_before_month[month - 1];
    if (month > 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)))
        days++;
    days += day - 1;
    days -= 719162;

    double seconds = (double)days * 86400.0;
    seconds += hour * 3600.0;
    seconds += minute * 60.0;
    seconds += second;
    seconds += ms / 1000.0;
    return seconds;
}

/* --------------------------------------------------------------------------
 * Shannon entropy aggregator
 * -------------------------------------------------------------------------- */
double calculate_cluster_distribution_entropy(const int *cluster_assignments,
                                              int total_rows,
                                              int num_clusters)
{
    if (!cluster_assignments || total_rows <= 0 || num_clusters <= 0)
        return 0.0;

    int *counts = calloc((size_t)num_clusters, sizeof(int));
    if (!counts)
        return 0.0;

    // Verify heap pointer alignment (e.g. 8-byte and 16-byte alignment checks)
    uintptr_t addr = (uintptr_t)counts;
    printf("[DEBUG] Allocation 'counts' address: %p (8-byte aligned: %s, 16-byte aligned: %s)\n",
           (void *)counts,
           (addr % 8 == 0) ? "YES" : "NO",
           (addr % 16 == 0) ? "YES" : "NO");

    for (int i = 0; i < total_rows; ++i) {
        int cid = cluster_assignments[i];
        if (cid >= 0 && cid < num_clusters)
            counts[cid]++;
    }

    double entropy = 0.0;
    const double inv_n = 1.0 / (double)total_rows;
    for (int c = 0; c < num_clusters; ++c) {
        double p = (double)counts[c] * inv_n;
        if (p > 1e-9)
            entropy -= p * log2(p);
    }

    SAFE_FREE(counts);
    return entropy;
}

/* --------------------------------------------------------------------------
 * Least-squares linear regression slope calculator
 * -------------------------------------------------------------------------- */
static bool least_squares_regression(const double *x, const double *y, size_t n,
                                     double *out_slope)
{
    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
    size_t valid_n = 0;

    for (size_t i = 0; i < n; ++i) {
        if (!isfinite(x[i]) || !isfinite(y[i]))
            continue;
        sum_x  += x[i];
        sum_y  += y[i];
        sum_xy += x[i] * y[i];
        sum_xx += x[i] * x[i];
        valid_n++;
    }

    if (valid_n < 2) {
        *out_slope = 0.0;
        return false;
    }

    double denom = (double)valid_n * sum_xx - sum_x * sum_x;
    if (fabs(denom) < 1e-12) {
        *out_slope = 0.0;
        return false;
    }

    *out_slope = ((double)valid_n * sum_xy - sum_x * sum_y) / denom;
    return true;
}

/* --------------------------------------------------------------------------
 * Minimal JSON field extractor
 * -------------------------------------------------------------------------- */
static int service_to_cluster(const char *service)
{
    if (strcmp(service, "auth-api") == 0)   return 0;
    if (strcmp(service, "payment-v2") == 0) return 1;
    if (strcmp(service, "mesh-router") == 0)return 2;
    return 0;
}

static bool parse_json_line(const char *line, LogEntry *out)
{
    memset(out, 0, sizeof(*out));

    const char *p = strstr(line, "\"timestamp\":\"");
    if (p) {
        p += strlen("\"timestamp\":\"");
        const char *q = strchr(p, '"');
        if (q) {
            size_t len = (size_t)(q - p);
            if (len >= sizeof(out->timestamp)) len = sizeof(out->timestamp) - 1;
            memcpy(out->timestamp, p, len);
            out->timestamp[len] = '\0';
        }
    }

    p = strstr(line, "\"service\":\"");
    if (p) {
        p += strlen("\"service\":\"");
        const char *q = strchr(p, '"');
        if (q) {
            size_t len = (size_t)(q - p);
            if (len >= sizeof(out->service)) len = sizeof(out->service) - 1;
            memcpy(out->service, p, len);
            out->service[len] = '\0';
        }
    }

    p = strstr(line, "\"latency_ms\":");
    if (p) {
        char *end = NULL;
        out->latency_ms = strtod(p + strlen("\"latency_ms\":"), &end);
        if (end == p + strlen("\"latency_ms\":")) out->latency_ms = 0.0;
    }

    p = strstr(line, "\"cpu_util\":");
    if (p) {
        char *end = NULL;
        out->cpu_util = strtod(p + strlen("\"cpu_util\":"), &end);
        if (end == p + strlen("\"cpu_util\":")) out->cpu_util = 0.0;
    }

    p = strstr(line, "\"mem_rss_mb\":");
    if (p) {
        char *end = NULL;
        out->mem_rss_mb = strtod(p + strlen("\"mem_rss_mb\":"), &end);
        if (end == p + strlen("\"mem_rss_mb\":")) out->mem_rss_mb = 0.0;
    }

    out->valid = (out->timestamp[0] != '\0');
    out->cluster_id = service_to_cluster(out->service);
    return out->valid;
}

/* --------------------------------------------------------------------------
 * Main — 100-row stress-test payload
 * -------------------------------------------------------------------------- */
int main(void)
{
    LogEntry entries[NUM_ROWS];
    for (int i = 0; i < NUM_ROWS; ++i) {
        if (!parse_json_line(RAW_LINES[i], &entries[i])) {
            fprintf(stderr, "FATAL: parse failure at row %d\n", i);
            return EXIT_FAILURE;
        }
    }

    int assignments[NUM_ROWS];
    for (int i = 0; i < NUM_ROWS; ++i)
        assignments[i] = entries[i].cluster_id;

    double entropy = calculate_cluster_distribution_entropy(
        assignments, NUM_ROWS, NUM_CLUSTERS);

    double base = parse_iso8601_to_seconds(entries[0].timestamp);
    double x[NUM_ROWS];
    double y_mem[NUM_ROWS], y_lat[NUM_ROWS], y_cpu[NUM_ROWS];
    size_t n = 0;

    for (int i = 0; i < NUM_ROWS; ++i) {
        x[i] = parse_iso8601_to_seconds(entries[i].timestamp) - base;
        y_mem[i] = entries[i].mem_rss_mb;
        y_lat[i] = entries[i].latency_ms;
        y_cpu[i] = entries[i].cpu_util;
        n++;
    }

    double slope_mem, slope_lat, slope_cpu;
    least_squares_regression(x, y_mem, n, &slope_mem);
    least_squares_regression(x, y_lat, n, &slope_lat);
    least_squares_regression(x, y_cpu, n, &slope_cpu);

    printf("Cluster Distribution Entropy : %.4f\n", entropy);
    printf("Trend Slope (mem_rss_mb)     : %.4f\n", slope_mem);
    printf("Trend Slope (latency_ms)     : %.4f\n", slope_lat);
    printf("Trend Slope (cpu_util)       : %.4f\n", slope_cpu);

    return EXIT_SUCCESS;
}
