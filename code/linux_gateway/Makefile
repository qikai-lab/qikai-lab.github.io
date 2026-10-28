CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wshadow -O2 -g -std=c11
CFLAGS  += -D_GNU_SOURCE
LDFLAGS ?=
# Ubuntu 上需要：sudo apt install build-essential libsqlite3-dev
LDLIBS  ?= -lsqlite3

TARGET  = gw
SRCS    = main.c gw_serial.c gw_frame.c gw_db.c gw_latest.c \
          gw_filter.c gw_alarm.c http_srv.c gw_cmd.c
OBJS    = $(SRCS:.c=.o)
HDRS    = gw_serial.h gw_frame.h gw_db.h gw_latest.h \
          gw_filter.h gw_alarm.h http_srv.h http_page.h gw_cmd.h

# 阶段 B：MQTT 订阅入库（独立程序，和 gw 是两条并行链路）
# Ubuntu 上需要：sudo apt install libmosquitto-dev
TARGET_MQTT = mqtt_sub
MQTT_LDLIBS = -lsqlite3 -lmosquitto

all: $(TARGET) $(TARGET_MQTT)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -I. -c -o $@ $<

# MQTT 订阅程序：单文件，不参与 gw 的 .o 集合，直接编译成可执行文件
$(TARGET_MQTT): mqtt_sub.c
	$(CC) $(CFLAGS) -o $@ mqtt_sub.c $(MQTT_LDLIBS)

# 改了 web/index.html 之后重新生成 http_page.h（需要 python3，平时不用跑）
web:
	python3 tools/build_page.py

# 离线单元测试：不需要串口、不需要板子、不需要 epoll
#   test_parse  解析层 + 落库层（50 项）
#   test_recent 历史查询 + 告警表（倒序 / 节点过滤 / 上限 / 重启清理）
#   test_filter 中值滤波 + 自适应基线 + 告警状态机（30 项）
#   test_http   HTTP 响应拼装 + 5 个接口的 JSON（重点：Content-Length 必须和 body 一致）
#   test_chain  端到端串联：假帧 → on_env → 滤波 → 告警 → 冻结 → 落库（M4 接线）
#   test_cmd    第 5 步：MQTT CONNECT/PUBLISH 报文逐字节断言（不需要 broker）
test: tests/test_parse tests/test_recent tests/test_filter tests/test_http tests/test_chain tests/test_cmd
	@echo ""
	./tests/test_parse
	@echo ""
	./tests/test_recent
	@echo ""
	./tests/test_filter
	@echo ""
	./tests/test_http
	@echo ""
	./tests/test_chain
	@echo ""
	./tests/test_cmd

tests/test_parse: tests/test_parse.c gw_frame.c gw_db.c $(HDRS)
	@mkdir -p tests
	$(CC) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/test_parse.c gw_frame.c gw_db.c $(LDLIBS)

tests/test_recent: tests/test_recent.c gw_db.c gw_frame.c $(HDRS)
	@mkdir -p tests
	$(CC) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/test_recent.c gw_db.c gw_frame.c $(LDLIBS)

# 这个测试不碰 sqlite / socket，纯逻辑，任何有 gcc 的机器都能跑
tests/test_filter: tests/test_filter.c gw_filter.c gw_alarm.c $(HDRS)
	@mkdir -p tests
	$(CC) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/test_filter.c gw_filter.c gw_alarm.c

# 注意：test_http.c 里 #include "http_srv.c"，所以这里**不能**再列 http_srv.c，
# 否则符号重复定义。这是白盒测试的常用手法（为了访问 static 的 build_response）。
# 但 http_srv.c 现在会调 gw_cmd_publish_default()（第 5 步的 /api/cmd），
# 所以必须带上 gw_cmd.c，否则链接期报 undefined reference。
tests/test_http: tests/test_http.c gw_latest.c gw_db.c gw_frame.c gw_filter.c gw_alarm.c gw_cmd.c $(HDRS)
	@mkdir -p tests
	$(CC) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/test_http.c gw_latest.c gw_db.c gw_frame.c gw_filter.c gw_alarm.c gw_cmd.c $(LDLIBS)

# test_chain.c 里 #define main gw_main_unused 后 #include "main.c"，
# 所以同样**不能**再列 main.c。它引用了 gw_serial_*，所以必须带上 gw_serial.c。
tests/test_chain: tests/test_chain.c gw_serial.c gw_frame.c gw_db.c gw_latest.c \
                  gw_filter.c gw_alarm.c http_srv.c gw_cmd.c $(HDRS)
	@mkdir -p tests
	$(CC) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/test_chain.c gw_serial.c gw_frame.c gw_db.c \
	      gw_latest.c gw_filter.c gw_alarm.c http_srv.c gw_cmd.c $(LDLIBS)

# test_cmd.c 里 #include "gw_cmd.c"（白盒，要访问 static 的 put_remlen），
# 所以这里**不能**再列 gw_cmd.c，否则符号重复。
# 它不需要 sqlite / socket，纯拼包，任何有 gcc 的机器都能跑。
tests/test_cmd: tests/test_cmd.c gw_cmd.c $(HDRS)
	@mkdir -p tests
	$(CC) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/test_cmd.c

# 装成 systemd 服务（M5）。需要 sudo，会生成 /etc/systemd/system/zigbee-gw.service
# 想换串口/端口：sudo ./install_service.sh /dev/ttyUSB1 9000
install-service: $(TARGET)
	@echo ">>> 需要 sudo，下面会提示输密码"
	@# 用 bash 显式执行，不依赖可执行位：从 Windows 打包过来时 .sh 常丢 +x，
	@# 那种情况下 `sudo ./install_service.sh` 会报 command not found（其实是 EACCES）。
	sudo bash ./install_service.sh

# 看服务日志（Ctrl+C 退出）
logs:
	journalctl -u zigbee-gw -f

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET_MQTT)
	rm -f tests/test_parse tests/test_recent tests/test_filter tests/test_http tests/test_chain
	rm -f tests/test_cmd
	rm -f tests/*.exe tests/*.db tests/*.db-wal tests/*.db-shm
	rm -f *.db *.db-wal *.db-shm

.PHONY: all test clean web install-service logs
