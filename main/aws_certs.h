#pragma once

#include <string.h>
#include <stdbool.h>

/* aws_certs.h - AWS IoT Core X.509 credentials (compile-time constants)
 *
 * ====================================================================
 *  ⚠️  THIS FILE IS COMMITTED TO A PUBLIC REPOSITORY.
 *      It must contain PLACEHOLDERS ONLY - the values below are fake.
 *
 *      Paste your own endpoint / Thing / certificates here when you build
 *      locally, and never commit them: a leaked private key lets someone
 *      else impersonate this device on your AWS account.
 *
 *      If real values were ever committed, REWRITING THIS FILE DOES NOT
 *      UNDO THE LEAK - the key stays in the git history. Rotate it in
 *      AWS IoT Core: create a new certificate for the Thing, attach the
 *      same policy, then deactivate (and delete) the old one.
 *
 *      To keep local edits out of `git status`:
 *          git update-index --skip-worktree main/aws_certs.h
 * ====================================================================
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
 * 手工粘贴 20 多行 base64 很容易漏 \n，建议用下面这段命令生成（WSL/Linux）：
 *
 *   cd examples/get-started/MPU_AWS_mqtt_REST_API
 *   cp main/aws_certs.h /tmp/aws_certs.tpl.h          # 先留一份占位符模板
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
 *     sed -n '/^static inline bool aws_certs_configured/,$p' /tmp/aws_certs.tpl.h
 *   } > /tmp/aws_certs.h && mv /tmp/aws_certs.h main/aws_certs.h
 *
 * 注意：宏定义意味着换证书必须重新编译烧写，这是有意的 —— 私钥不进 NVS，
 * 也就不必经过配网热点的明文 HTTP。
 */

/* ==================================================================== */
/*  0. AWS IoT 端点与 Thing —— 跟着证书一起编译进固件，不参加配网        */
/* ==================================================================== */
/* 取自 IoT 控制台 *Settings -> Device data endpoint*：
 *   xxxxxxxxxxxxxx-ats.iot.<region>.amazonaws.com
 * 只写域名，别带 https:// 或端口（端口见 AWS_IOT_PORT）。
 *
 * 注意 v 和 z 的顺序是 vmz，很容易看成 vzm —— 抄错一个字母的现象是
 * TCP 能连上但 TLS 握手立刻被断（EOF），而不是给出证书错误。
 * tools/aws_check.py 可以在烧写前先在 PC 上验证端点是否真的可用。
 *
 * 这两个是默认值，正常路径下**就是实际生效的值**：配网页只写 ssid/pass，
 * NVS 里的 aws_ep / thing 永远是空的，所以 cfg_load() 每次都回落到这里。
 * 改了宏必须重新 build + flash 才生效。
 *
 * 别用"长按 BOOT 3 秒清空配置"来刷新端点 —— cfg_erase() 是 nvs_erase_all，
 * 会把 WiFi 一起抹掉，结果是要重新配一次网。 */
#define AWS_IOT_ENDPOINT "REPLACE_WITH_xxxxxxxxxxxxxx-ats.iot.<region>.amazonaws.com"
#define AWS_IOT_PORT     8883
#define AWS_IOT_THING    "REPLACE_WITH_your_thing_name"

/* ==================================================================== */
/*  1. 根 CA —— AmazonRootCA1.pem                                       */
/* ==================================================================== */
/* 这一份本身是公开值（Amazon Root CA 1，可从
 * https://www.amazontrust.com/repository/AmazonRootCA1.pem
 * 取得，不需要保密）；这里照样留占位符，纯粹是为了不让"某台真实设备用过的
 * 那一套"混进示例。想省事可以把它替换成真正的 AmazonRootCA1.pem 再提交。 */
#define AWS_IOT_CA_PEM \
    "REPLACE_WITH_AmazonRootCA1_PEM\n"

/* ==================================================================== */
/*  2. 设备证书 —— xxxxxxxx-certificate.pem.crt（与 Thing 绑定）         */
/* ==================================================================== */
#define AWS_IOT_CERT_PEM \
    "REPLACE_WITH_DEVICE_CERTIFICATE_PEM\n"

/* ==================================================================== */
/*  3. 设备私钥 —— xxxxxxxx-private.pem.key（敏感，勿外泄、勿提交）      */
/* ==================================================================== */
#define AWS_IOT_KEY_PEM \
    "REPLACE_WITH_DEVICE_PRIVATE_KEY_PEM\n"

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
