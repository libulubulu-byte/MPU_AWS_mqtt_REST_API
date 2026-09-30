#pragma once

#include <string.h>
#include <stdbool.h>

/* aws_certs.h - AWS IoT Core X.509 证书（编译期常量）
 *
 * 从 AWS IoT 控制台 *Create thing* 时下载三个文件，把内容填进下面三个宏：
 *
 *   宏名                  AWS 下载的文件名                用途
 *   AWS_IOT_CA_PEM        AmazonRootCA1.pem               根 CA（验证 broker）
 *   AWS_IOT_CERT_PEM      xxxxxxxx-certificate.pem.crt   设备证书
 *   AWS_IOT_KEY_PEM       xxxxxxxx-private.pem.key       设备私钥
 *
 * 每个宏是多段字符串字面量拼接，**每行必须以 \n 结尾**：PEM 靠换行分隔
 * base64 行，少一个换行 mbedTLS 就解析失败（报 TLS 握手错误）。
 *
 * 另外一个同样致命的坑：正文行（每个宏的最后一行除外）必须**正好 64 个
 * 字符**。少一个字符 base64 就整体错位、私钥解不出来，现象和少 \n 一样是
 * TLS 握手失败。粘完请用下面的命令核对行长，别靠肉眼数：
 *
 *   awk -F'"' '/^    "/ {printf "%4d: %d\n", NR, length($2)-2}' main/aws_certs.h
 *   # 输出的第二列除每段末尾外应全是 64
 *
 * 三份都是敏感内容，私钥泄露等于设备身份被盗用 —— 别提交到公开仓库。
 *
 * 手工粘贴 20 多行 base64 很容易漏 \n，建议用下面这段命令生成（WSL/Linux）：
 *
 *   cd examples/get-started/AWS_mqtt
 *   {
 *     echo '#pragma once'; echo; echo '#include <string.h>'
 *     echo '#include <stdbool.h>'; echo
 *     for pair in "AWS_IOT_CA_PEM:AmazonRootCA1.pem" \
 *                 "AWS_IOT_CERT_PEM:xxxxxxxx-certificate.pem.crt" \
 *                 "AWS_IOT_KEY_PEM:xxxxxxxx-private.pem.key"; do
 *       macro=${pair%%:*}; file=${pair#*:}
 *       echo "#define $macro \\"
 *       sed 's/\\/\\\\/g; s/"/\\"/g; s/$/\\n" \\/' "$file" | sed 's/^/    "/'
 *       echo '    ""'; echo
 *     done
 *     sed -n '/^static inline bool aws_certs_configured/,$p' main/aws_certs.h
 *   } > /tmp/aws_certs.h && mv /tmp/aws_certs.h main/aws_certs.h
 *   # 注意：脚本里那句 echo 会输出 C 注释包裹文件名，此处省略不写
 *
 *
 * 注意：宏定义意味着换证书必须重新编译烧写，这是有意的 —— 私钥不进 NVS，
 * 也就不必经过配网热点的明文 HTTP。
 */

/* ==================================================================== */
/*  0. AWS IoT 端点 —— 跟着证书一起编译进固件，不参加配网                */
/* ==================================================================== */
/* 取自 IoT 控制台 *Settings -> Device data endpoint*：
 *   a1ilmvmz892cea-ats.iot.ap-southeast-2.amazonaws.com
 * 只写域名，别带 https:// 或端口（端口见 AWS_IOT_PORT）。
 *
 * 注意 v 和 z 的顺序是 vmz，很容易看成 vzm —— 抄错一个字母的现象是
 * TCP 能连上但 TLS 握手立刻被断（EOF），而不是给出证书错误。
 * tools/aws_check.py 可以在烧写前先在 PC 上验证端点是否真的可用。
 *
 * 这三个是默认值，正常路径下**就是实际生效的值**：配网页只写 ssid/pass，
 * NVS 里的 aws_ep / thing 永远是空的，所以 cfg_load() 每次都回落到这里。
 * 改了宏必须重新 build + flash 才生效。
 *
 * 别用"长按 BOOT 3 秒清空配置"来刷新端点 —— cfg_erase() 是 nvs_erase_all，
 * 会把 WiFi 一起抹掉，结果是要重新配一次网。 */
#define AWS_IOT_ENDPOINT "a1ilmvmz892cea-ats.iot.ap-southeast-2.amazonaws.com"
#define AWS_IOT_PORT     8883
#define AWS_IOT_THING    "ESP32S3"

/* ==================================================================== */
/*  1. 根 CA —— AmazonRootCA1.pem                                       */
/* ==================================================================== */
#define AWS_IOT_CA_PEM \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n" \
    "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n" \
    "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n" \
    "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n" \
    "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n" \
    "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n" \
    "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n" \
    "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n" \
    "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n" \
    "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n" \
    "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n" \
    "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n" \
    "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n" \
    "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n" \
    "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n" \
    "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n" \
    "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n" \
    "rqXRfboQnoZsG4q5WTP468SQvvG5\n" \
    "-----END CERTIFICATE-----\n"

/* ==================================================================== */
/*  2. 设备证书 —— xxxxxxxx-certificate.pem.crt                         */
/* ==================================================================== */
#define AWS_IOT_CERT_PEM \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIDWTCCAkGgAwIBAgIUQCPEN3+TJgK7XoUesXHEfIrAahQwDQYJKoZIhvcNAQEL\n" \
    "BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g\n" \
    "SW5jLiBMPVNlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTI2MDkxMzA5MDM1\n" \
    "MloXDTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0\n" \
    "ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMJHH8GyNiYegMEoDMNL\n" \
    "3210uSUchVYRASJ1kgcxavomb/2R5v/7ExOBiRNjyrRbP5q3fjLlmHt5/R4UCn8N\n" \
    "d6SbxTL26CkCS866F07c3U2cDQJR1L8LKaAx2d6cw4OfGe02hsGg3dcMHWnZClO8\n" \
    "sMxG0vCVcBTduZt8C2FZb+AfAYj/NvhG9GOs1JYbtNYHIvjLEZMHELJ7jF3hscY2\n" \
    "zPTJrNQqzxJbvjxddn/eKd2rgorTtE+u+OHymGc+H3XmG8vn3UEQVlIomIaLOQgP\n" \
    "avZrc1RV3292pea8P2qWSUbalxsNzUbYhpqzjyMmrJ3+oQWaiGE2wo9CGrBt1sNS\n" \
    "YU0CAwEAAaNgMF4wHwYDVR0jBBgwFoAUvOIXksb7GakbnwcdlqOOeK2vKP4wHQYD\n" \
    "VR0OBBYEFLbuJM+dccLp0C45hDxEGyNJIXOnMAwGA1UdEwEB/wQCMAAwDgYDVR0P\n" \
    "AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQBPJeWkTILRjU+8jk2GkyrXc3oY\n" \
    "euJd162G44KKA03pgqUMp0OfcGqC85a316rHR5eNsaV9UNWQoycrZf3EDrHfqKDv\n" \
    "IH5EObj+nZr4f4VwfRN8wzxLMi2G5ZEHDDmHgq68+16ijCaOWzeb9REjPwk9udeZ\n" \
    "hMN+iEu2AQSZTj3rAlpwD48nMy9RQwZlq2s2IEQiANOtD/sIuc/ljlfhpNnSRLgD\n" \
    "gcGrFZm24ZuwdOqzn+Q0WPwM95/XCXuNMRWcbCQ7Nqg7bmfV/9Pj3ojkfJGVRZ4D\n" \
    "en6T7TY3NKd/G6BvJuiEWTwyToF2AAPoGmTkmHXAFgcNdHyPjCPoZch5aeke\n" \
    "-----END CERTIFICATE-----\n"

/* ==================================================================== */
/*  3. 设备私钥 —— xxxxxxxx-private.pem.key（敏感，勿外泄）              */
/* ==================================================================== */
#define AWS_IOT_KEY_PEM \
    "-----BEGIN RSA PRIVATE KEY-----\n" \
    "MIIEogIBAAKCAQEAwkcfwbI2Jh6AwSgMw0vfbXS5JRyFVhEBInWSBzFq+iZv/ZHm\n" \
    "//sTE4GJE2PKtFs/mrd+MuWYe3n9HhQKfw13pJvFMvboKQJLzroXTtzdTZwNAlHU\n" \
    "vwspoDHZ3pzDg58Z7TaGwaDd1wwdadkKU7ywzEbS8JVwFN25m3wLYVlv4B8BiP82\n" \
    "+Eb0Y6zUlhu01gci+MsRkwcQsnuMXeGxxjbM9Mms1CrPElu+PF12f94p3auCitO0\n" \
    "T6744fKYZz4fdeYby+fdQRBWUiiYhos5CA9q9mtzVFXfb3al5rw/apZJRtqXGw3N\n" \
    "RtiGmrOPIyasnf6hBZqIYTbCj0IasG3Ww1JhTQIDAQABAoIBAGlTOeo+/ZBI9TDC\n" \
    "z7iJR8YFg+KUxczVRzIxX0u7BO49LHXiRcP9kGgA0BnM/jYtShxM4oQhaTt39Tv7\n" \
    "TX14BaceNBjfNxoUfpjC1qQQpYHP2lWpm5c/LAAHRsCjaHPRK7Mo3oHW/q35iXC7\n" \
    "FBg1sfvtYarAuZT2aAvdgiXy4O41rkC+W80pBOsfE3bv0W9mvVdtqWhwvMwZXlfc\n" \
    "HOhH/O+HJ7twS3k/rzzm1NXZd/vzAnnf9wjalIAOmlplzpD6PrdK/klYJipWrB4p\n" \
    "YhIiINUIwybl/NZxSXNPKQntsgpSjw9wQ4aD0x69j+ediiVRWszudP1Ex82G7r6d\n" \
    "VSR5DUECgYEA9HNPh0jMbkv1gXwUOnpG8A1G55mpOZuhJ20TfAmf7hXAB0XAMBV2\n" \
    "V11Z8H86f40rfOiIjyMaWE85KJkZOo1mmraYQTWcBL83/0YwiS1AUq/WLjsRWmZw\n" \
    "2UafTY2qH20hOW/CNFqlTZ1o8qxn1HR3LZwflXoWVVedVOQNEefCW2UCgYEAy3T2\n" \
    "kR/jTQqAeX2jhmk81CgA854TbjvFcD5x+mRZKBuwsmf0fiZju+NWHHEUxfyNdrwG\n" \
    "+khz9q9SK048k+5FUUIzioxSs+vRH4Vl2nDZIcOGGC8etXNxNwNH3ufPu2jM2me6\n" \
    "4bB5TYUluv2sdV9/3sCydUZ7Tha5n1rS5pf9s8kCgYBsc8xQ5Qdt191wOTBwUVev\n" \
    "oPRYRGBD4rw41bfTTCHca2Hq2BNQQfVjBVOl04yMkoE8xZ3wg05o550gWexLgfiH\n" \
    "o4MhSzuRD5U0eFuIQL9M8B5CHIqyZMikXSTIL5XOo4geB2tN9vln2fJ51+uo4pMN\n" \
    "dTq+ApBuBJUU2KjDzOL50QKBgC7dak5TghAk8yMJBbnYU+KqtE1phCBINFp+h9Kd\n" \
    "esv6VOFDgNXuEdsdqqAyjA5u9kb7WMAeIFgaWlsPUnTg1aa6ERVA7Wv0Td9s4uFG\n" \
    "TT2xxBmeAPza8qExaES4MOmCYm2Mp0eFVuu8V8yS0j7XGKU+zdylt5FtOqSyTxI+\n" \
    "wHnBAoGAUdE2IcLWx7ttvEgClWq8IMEPZYGne956JqTFbB9RQoIWfVVpmhEwWRT1\n" \
    "cczuld7DWzpvGW5T9NT8/zYZO5wcoPnDKtOrzAAgx4iJzFNoc8CMsvXZnHArDMF9\n" \
    "ctjjtC3zNuVLaLLuRgsf8LpXXDpVcb4iZi/fWrAWW2/b9jMwe8c=\n" \
    "-----END RSA PRIVATE KEY-----\n"

/* -------------------------------------------------------------------- */
/*  占位符自检：三份都填过就返回 true，否则启动时报错，                */
/*  而不是拿着一堆假证书反复 TLS 握手失败。                             */
/* -------------------------------------------------------------------- */
static inline bool aws_certs_configured(void)
{
    return strstr(AWS_IOT_CA_PEM, "REPLACE_WITH_") == NULL &&
           strstr(AWS_IOT_CERT_PEM, "REPLACE_WITH_") == NULL &&
           strstr(AWS_IOT_KEY_PEM, "REPLACE_WITH_") == NULL;
}
