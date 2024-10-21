// update : 24.08.22 AM 10:58
// update : 24.08.22 AM 09:15
// update : 24.08.23 PM 16:34
// update : 24.09.05 PM 16:52
// update : 24.09.06 am 11:12
// update : 24.09.09 am 10:56
// update : 24.09.10 Am 11:46
// update : 24.09.11 AM 09:16
// update : 24.09.11 AM 09:45


#include "LoRa.h"
#include <nfc/nfc.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mosquitto.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

// 마스터의 보안 ID 및 주소와 다음 슬레이브 주소
unsigned char MySecurityID[2];
unsigned char ADDRESS = 127;
unsigned char next_slaveADDRESS = 1;

// 로그를 저장할 경로와 로그 메시지
const char *folderPath = "./logs";  // 장치 로그 파일 경로
const char *m_logPath = "./m_logs"; // 마스터 간 통신 로그 파일 경로
char logMessage[256];               // 로그 메시지를 저장할 버퍼

// NFC 태그 감지 및 데이터 전송을 위한 pthread 변수
pthread_t threads[2];
unsigned char thread_status = 0;

// 전송 프레임 구조체 정의
typedef struct {
    unsigned char securityID[2];
    unsigned char address;
    unsigned char seq;
    unsigned char command;
    unsigned char parameter;
    unsigned char reserved[2];
} Frame;

// 전송 및 수신을 위한 변수들
unsigned char ack_receive;
unsigned char is_broadcast;
unsigned char transmit_period = 0;

// 송신 프레임 초기화
Frame tx_frame = {
    .seq = 0};

// LoRa 및 MQTT용 변수들
LoRa_ctl modem;
struct mosquitto *mosq;

// 등록된 슬레이브 주소를 저장할 최대 개수 및 배열
#define MAX_DEVICES 100
unsigned char registered_addresses[MAX_DEVICES];
unsigned int registered_count = 0;

// 마스터 식별 ID 및 상태 정의
#define MASTER1_ID 0x01
#define MASTER2_ID 0x02
unsigned char MASTER_ID = MASTER1_ID; // 현재 마스터의 ID
char *master_status = "idle";         // 기본 상태는 idle

// 마스터 통신 시작/종료 상태
bool master_communication_active = false;

// LED 제어 관련 상수 정의
#define LED3_PIN 3                // LED 3에 해당하는 핀 번호
#define COMMAND_LED_CONTROL 0x18  // LED 제어 명령어 정의

// 로그 작성 함수 선언
void writeLog(const char *folderPath, const char *tag, const char *logMessage);
void writeMasterLog(const char *tag, const char *logMessage);  // 마스터 통신 로그 함수 선언

// 메시지 전송 함수 선언
void send_message(const char *message, unsigned char address, const char *mac, const unsigned char *uid, size_t uid_len);

// 로그 파일 작성 함수
void writeLog(const char *folderPath, const char *tag, const char *logMessage) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char filePath[256];
    snprintf(filePath, sizeof(filePath), "%s/log_%04d-%02d-%02d.txt", folderPath, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);

    struct stat st = {0};
    if (stat(folderPath, &st) == -1) {
        mkdir(folderPath, 0777);
    }

    FILE *file = fopen(filePath, "a");
    if (file == NULL) {
        perror("Unable to open log file");
        return;
    }
    fprintf(file, "[%04d-%02d-%02d %02d:%02d:%02d] %s %s\n",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec, tag, logMessage);

    fclose(file);
}

// 마스터 통신 로그 작성 함수
void writeMasterLog(const char *tag, const char *logMessage) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char filePath[256];
    snprintf(filePath, sizeof(filePath), "%s/m_log_%04d-%02d-%02d.txt", m_logPath, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);

    struct stat st = {0};
    if (stat(m_logPath, &st) == -1) {
        mkdir(m_logPath, 0777);
    }

    FILE *file = fopen(filePath, "a");
    if (file == NULL) {
        perror("Unable to open master log file");
        return;
    }
    fprintf(file, "[%04d-%02d-%02d %02d:%02d:%02d] %s %s\n",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec, tag, logMessage);

    fclose(file);
}

// 전송 프레임 데이터를 포함한 로그 메시지를 생성하는 함수
void make_logMessage(Frame *frame, char *logMessage) {
    sprintf(logMessage, "Security ID: %02x %02x Address: %02x Seq: %d Command: %02x Parameter: %02x",
            frame->securityID[0], frame->securityID[1], frame->address, frame->seq, frame->command, frame->parameter);
}

// 새로운 주소를 등록하는 함수
void register_address(unsigned char address) {
    for (unsigned int i = 0; i < registered_count; i++) {
        if (registered_addresses[i] == address) {
            printf("Address 0x%02x is already registered.\n", address);
            return;
        }
    }

    if (registered_count < MAX_DEVICES) {
        registered_addresses[registered_count++] = address;
        printf("Address 0x%02x registered successfully.\n", address);
    } else {
        printf("Address list is full. Cannot register new address.\n");
    }
}

// 통신 완료 후 slave에게 LED 제어 명령을 전송하는 함수
void notify_slave_to_blink_led() {
    tx_frame.command = COMMAND_LED_CONTROL;           // LED 제어 명령 설정
    tx_frame.parameter = (1 << 6) | (0 << 3) | 3;     // 1초 켜짐, 반복 없음, 3번 반복
    make_logMessage(&tx_frame, logMessage);           // 로그 메시지 생성
    writeLog(folderPath, "TX", logMessage);           // 로그 기록 추가
    LoRa_send(&modem);                                // LoRa 전송
}

////////////// 스레드 함수 정의 //////////////
// 데이터 전송을 위한 스레드 함수
void *transmit(void *arg) {
    struct timespec ts;  // 타이머 구조체
    clock_gettime(CLOCK_MONOTONIC, &ts);  // 현재 시간을 모노토닉 시계로 가져오기
    while (true) {
        memcpy(modem.tx.data.buf, &tx_frame, sizeof(Frame));  // 전송 프레임을 버퍼에 복사
        make_logMessage(&tx_frame, logMessage);  // 로그 메시지 생성

        writeLog(folderPath, "TX", logMessage);  // 전송 로그 기록

        struct timeval tv;  // 현재 시간 저장
        struct tm *tm;  // 시간 구조체
        char time_str[100];  // 시간 문자열 버퍼
        gettimeofday(&tv, NULL);  // 현재 시간 가져오기
        tm = localtime(&tv.tv_sec);  // 시간을 로컬 시간으로 변환
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);  // 시간 문자열 생성
        printf("  Time(sec): %ld.%06ld\n", tv.tv_sec, tv.tv_usec);  // 전송 시간을 출력
        LoRa_send(&modem);  // LoRa를 통해 데이터 전송

        tx_frame.seq++;  // 프레임 시퀀스 증가
        while (ack_receive == 0) { }  // ACK 메시지를 기다림
        ack_receive = 0;  // ACK 수신 상태 초기화
        if (transmit_period == 0) { break; }  // 한 번 전송 후 종료
        ts.tv_sec += transmit_period;  // 다음 전송 시간을 설정
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);  // 지정된 시간 동안 대기
    }
    thread_status = 0;  // 전송 스레드 상태 초기화
    return NULL;
}

// NFC 태그 감지를 위한 스레드 함수
void *nfc_tag() {
    nfc_device *pnd;  // NFC 디바이스 핸들
    nfc_target nt;  // NFC 타겟 구조체
    nfc_context *context;  // NFC 컨텍스트 구조체 포인터

    // NFC 라이브러리 초기화
    nfc_init(&context);  
    if (context == NULL) {
        printf("Unable to init libnfc (malloc)\n");
        exit(EXIT_FAILURE);  // 초기화 실패 시 프로그램 종료
    }

    // NFC 디바이스 열기
    pnd = nfc_open(context, NULL);  
    if (pnd == NULL) {
        printf("ERROR: %s\n", "Unable to open NFC device.");
        exit(EXIT_FAILURE);  // 디바이스 열기 실패 시 프로그램 종료
    }

    // NFC 디바이스를 이니시에이터 모드로 설정
    if (nfc_initiator_init(pnd) < 0) {  
        nfc_perror(pnd, "nfc_initiator_init");
        exit(EXIT_FAILURE);  // 초기화 실패 시 프로그램 종료
    }

    // ISO14443A (MIFARE) 태그를 위한 폴링 설정
    const nfc_modulation nmMifare = {  
        .nmt = NMT_ISO14443A,
        .nbr = NBR_106,
    };
    nfc_device_set_property_bool(pnd, NP_INFINITE_SELECT, false);  // NFC 태그 오류를 출력하지 않도록 설정
    
    while (1) {
        // NFC 태그 감지
        while (!(nfc_initiator_select_passive_target(pnd, nmMifare, NULL, 0, &nt) > 0)) { }

        if(nt.nti.nai.szUidLen > 0) {
            printf("\nDetected NFC Tag UID (NFCID%c): ", (nt.nti.nai.abtUid[0] == 0x08 ? '3' : '1'));  // UID 출력
            unsigned char detected_address = 0;
            for (size_t szPos = 0; szPos < nt.nti.nai.szUidLen; szPos++) {
                printf("%02x  ", nt.nti.nai.abtUid[szPos]);  // UID의 각 바이트를 출력
                detected_address ^= nt.nti.nai.abtUid[szPos];  // UID의 각 바이트를 XOR하여 address 생성
            }
            printf("\nDetected Address: 0x%02x\n", detected_address);  // 생성된 주소 출력

            // 주소 등록
            register_address(detected_address);  

            // 새로운 장치 등록 메시지 생성 및 전송, NFC의 UID도 함께 전송
            send_message("Register", detected_address, "192.168.3.45", nt.nti.nai.abtUid, nt.nti.nai.szUidLen);
        }

        // 동일한 UID를 계속 감지하는 것을 방지
        while (nfc_initiator_select_passive_target(pnd, nmMifare, NULL, 0, &nt) > 0) { }
    }

    // NFC 디바이스 닫기 및 종료
    nfc_close(pnd);  
    nfc_exit(context);  
    exit(EXIT_FAILURE);
}

////////////// Mosquitto 함수 정의 //////////////
// MQTT 브로커에 연결되었을 때 호출되는 함수
void on_connect(struct mosquitto *mosq, void *obj, int rc) {
    if (rc == 0) {
        mosquitto_subscribe(mosq, NULL, "lora/command", 0);  // 토픽 구독
    } else {
        printf("Failed to connect, return code %d\n", rc);  // 연결 실패 시 오류 코드 출력
    }
}

// MQTT 메시지를 수신했을 때 호출되는 함수
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
    printf("Received message: %s\n", (char *)msg->payload);  // 수신된 메시지 출력 (디버깅 용도)
    if (thread_status == 1) {
        pthread_cancel(threads[1]);  // 전송 스레드 취소
        pthread_join(threads[1], NULL);  // 스레드 종료 대기
    }
    
    // 수신된 JSON 메시지 파싱
    json_object *jobj = json_tokener_parse((char *)msg->payload);
    json_object *address, *command, *led, *params, *period;

    unsigned char r_address, r_command, r_LED = 0, r_time, r_cycle, r_count, r_period;
    // address 필드 추출
    if (json_object_object_get_ex(jobj, "address", &address)) {
        r_address = (unsigned char)json_object_get_int(address);
    }
    if (r_address == 0xFF) { is_broadcast = 1; }  // 브로드캐스트 메시지인지 확인
    else { is_broadcast = 0; }
    // command 필드 추출
    if (json_object_object_get_ex(jobj, "command", &command)) {
        const char *cmd_str = json_object_get_string(command);
        if (strcmp(cmd_str, "start") == 0) {
            // 통신 시작 명령 수신 처리
            if (!master_communication_active) {
                master_communication_active = true;
                printf("Communication between masters started.\n");
                send_message("CommStart", r_address, "", NULL, 0);
            }
            json_object_put(jobj);
            return;
        } else if (strcmp(cmd_str, "stop") == 0) {
            // 통신 종료 명령 수신 처리
            if (master_communication_active) {
                master_communication_active = false;
                printf("Communication between masters stopped.\n");
                send_message("CommStop", r_address, "", NULL, 0);
            }
            json_object_put(jobj);
            return;
        } else {
            r_command = (unsigned char)atoi(cmd_str);  // 기존 숫자 명령 처리
        }
    }

    // 명령이 5인 경우 데이터 송신을 중단하고 원래 상태로 돌아감
    if (r_command == 5) {
        json_object_put(jobj);  // JSON 객체 해제
        printf("Command 5 received, returning to idle state.\n");  // 명령 5 수신 메시지 출력
        return;  // 데이터 송신 중단하고 함수 종료
    }

    // led 필드 추출
    if (json_object_object_get_ex(jobj, "led", &led)) {
        json_object *red, *green, *blue, *motor;
        if (json_object_object_get_ex(led, "RED", &red)) {
            r_LED |= json_object_get_int(red);
        }
        if (json_object_object_get_ex(led, "GREEN", &green)) {
            r_LED |= json_object_get_int(green) << 1;
        }
        if (json_object_object_get_ex(led, "BLUE", &blue)) {
            r_LED |= json_object_get_int(blue) << 2;
        }
        if (json_object_object_get_ex(led, "MOTOR", &motor)) {
            r_LED |= json_object_get_int(motor) << 3;
        }
    }
    // params 필드 추출
    if (json_object_object_get_ex(jobj, "params", &params)) {
        json_object *turn_on_time, *cycle, *count;
        if (json_object_object_get_ex(params, "turn_on_time", &turn_on_time)) {
            r_time = (unsigned char)(json_object_get_double(turn_on_time)/0.25);
        }
        if (json_object_object_get_ex(params, "cycle", &cycle)) {
            r_cycle = (unsigned char)json_object_get_int(cycle) - 1;
        }
        if (json_object_object_get_ex(params, "count", &count)) {
            r_count = (unsigned char)json_object_get_int(count);
        }
    }
    // period 필드 추출
    if (json_object_object_get_ex(jobj, "period", &period)) {
        r_period = (unsigned char)json_object_get_int(period);
    }

    // 전송 시작
    // tx_frame 설정
    tx_frame.address = r_address;

    if (r_time == 0) { r_command = 3; }  // turn on 명령으로 설정
    else { r_time -= 1; }  // 시간 값 설정 (0~3)
    tx_frame.command = (r_command << 4) | r_LED;  // 명령 및 LED 설정
    
    if (r_command == 3 ) { tx_frame.parameter = ((r_cycle+1) * r_count * 4) + 1; }  // parameter 설정
    else if (r_command == 1) { tx_frame.parameter = (r_time << 6) | (r_cycle << 3) | r_count; }  // parameter 설정
    else { tx_frame.parameter = 0; }  // 기타 명령의 경우 parameter는 0으로 설정

    transmit_period = r_period;  // 전송 주기 설정
    memcpy(modem.tx.data.buf, &tx_frame, sizeof(Frame));  // txbuf에 프레임 복사

    json_object_put(jobj);  // JSON 객체 해제

    // 전송 스레드 시작
    thread_status = 1;
    pthread_create(&threads[1], NULL, transmit, NULL);  // 전송 스레드 생성
    pthread_detach(threads[1]);  // 전송 스레드 분리
}

// 메시지를 JSON 형식으로 변환하여 전송하는 함수
void send_message(const char *message, unsigned char address, const char *mac, const unsigned char *uid, size_t uid_len) {
    char payload[512];  // 메시지를 저장할 버퍼
    int rc;
    
    // UID를 문자열로 변환
    char uid_str[64] = {0};
    for (size_t i = 0; i < uid_len; i++) {
        snprintf(uid_str + i * 2, sizeof(uid_str) - i * 2, "%02x", uid[i]);
    }

    // Master ID를 문자열로 설정 ("1" 또는 "2")
    const char *master_id_str = (MASTER_ID == MASTER1_ID) ? "1" : "2";

    // Master 간 통신 메시지인 경우 "node" 필드를 추가하여 Master 1 또는 Master 2를 구분
    if (strcmp(message, "MTx") == 0 || strcmp(message, "MRx") == 0) {
        const char *node = (MASTER_ID == MASTER1_ID) ? "master_1" : "master_2";  // Master ID에 따라 노드를 설정
        snprintf(payload, sizeof(payload), "{\"type\": \"%s\", \"Maddress\": %d, \"node\": \"%s\", \"masterID\": \"%s\"}", message, address, node, master_id_str);
    } 
    // NFC 태그 등록 메시지의 경우 "masterID"를 추가
    else if (strcmp(message, "Register") == 0) {
        snprintf(payload, sizeof(payload), "{\"type\": \"%s\", \"address\": %d, \"mac\": \"%s\", \"uid\": \"%s\", \"masterID\": \"%s\"}", message, address, mac, uid_str, master_id_str);
    } 
    // 일반 메시지인 경우 "MTx"를 "Tx"로 수정
    else {
        if (strcmp(message, "MTx") == 0) {
            message = "Tx";  // MTx 메시지를 Tx로 변경
        }
        snprintf(payload, sizeof(payload), "{\"type\": \"%s\", \"address\": %d, \"mac\": \"%s\", \"uid\": \"%s\"}", message, address, mac, uid_str);
    }

    // MQTT를 통해 메시지 전송
    rc = mosquitto_publish(mosq, NULL, "lora/status", strlen(payload), payload, 0, false);  // 지정된 토픽으로 전송
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Failed to publish message: %s\n", mosquitto_strerror(rc));  // 전송 실패 시 오류 메시지 출력
    }
    printf("Sent message: %s\n", payload);  // 전송된 메시지 출력
}


////////////// Ack Timeout 함수 정의 //////////////
// Ack 메시지 수신 타임아웃 시 호출되는 함수
void timeout_handler(int signum) {
    if (!ack_receive) {  // ACK 수신 실패
        printf("Timeout: No response received.\n");  // 타임아웃 메시지 출력
        sprintf(logMessage, "Address %02x device is inactive", tx_frame.address);  // 로그 메시지 생성
        writeLog(folderPath, "No Ack", logMessage);  // 로그 파일에 기록
        send_message("NoAck", tx_frame.address, "", NULL, 0);  // "NoAck" 메시지 전송
    }
}

// ACK 수신 타임아웃 설정 함수
void set_timeout(int seconds) {
    struct sigaction sa;  // 신호 처리 구조체
    struct itimerval timer;  // 타이머 구조체

    // 타임아웃 핸들러 설정
    sa.sa_handler = &timeout_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    // 타이머 설정
    timer.it_value.tv_sec = seconds;  // 타이머 값 설정
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &timer, NULL);  // 타이머 시작
}

////////////// SecurityID 및 주소 처리 함수 //////////////
// 보안 ID를 무작위로 생성하는 함수
void set_securityID(unsigned char *securityID) {
    srand(time(NULL));  // 난수 생성기 초기화
    for (int i = 0; i < 2; i++) {
        // 보안 ID를 랜덤 값으로 설정
        securityID[i] = 0xFF;  // 테스트를 위해 고정된 값으로 설정
    }
}

// 수신된 보안 ID를 확인하는 함수
int check_ID(unsigned char receiveID[2]) {
    return ((receiveID[0] == MySecurityID[0] && receiveID[1] == MySecurityID[1]) || 
            (receiveID[0] == 0xFF && receiveID[1] == 0xFF));  // 받은 ID가 내 ID와 같거나 0xFF이면 true 반환
}

// 수신된 주소를 확인하는 함수
int check_address(unsigned char receive_address) {
    if (receive_address == ADDRESS) { return 1; }  // 받은 주소가 내 주소와 같으면 true 반환
    return 0;
}

///////////////Master to Slave/////////////////////////
////////////// 전송 및 수신 콜백 함수 정의 //////////////
// 전송 콜백 함수
void tx_f(txData *tx) {
    LoRa_ctl *modem = (LoRa_ctl *)(tx->userPtr);  // LoRa 모뎀 포인터 가져오기
    writeLog(folderPath, "TX", logMessage);  // 전송 로그 작성
    send_message("Tx", tx_frame.address, "", NULL, 0);  // 전송 메시지 전송
    if (is_broadcast == 0) {  // 브로드캐스트 메시지가 아니면 ACK 대기
        LoRa_receive(modem);  // 수신 모드로 전환
        set_timeout(6);  // 6초 타임아웃 설정
    } else { 
        ack_receive = 1;  // 브로드캐스트인 경우 ACK 수신 상태로 설정
    } 
}

// 수신 콜백 함수
void *rx_f(void *p){
    rxData *rx = (rxData *)p;  // 수신된 데이터 포인터 가져오기
    LoRa_ctl *modem = (LoRa_ctl *)(rx->userPtr);  // LoRa 모뎀 포인터 가져오기
    Frame * frame = (Frame *)(rx->buf);  // 수신된 프레임 데이터 포인터 가져오기
    // 보안 ID 및 주소 확인
    if ((!check_ID(frame->securityID)) || (!check_address(frame->address))) { 
        free(p);  // 내 ACK가 아닌 경우 메모리 해제
        return NULL;  // 함수 종료
    }  
    ack_receive = 1;  // ACK 수신 상태로 설정
    set_timeout(0);  // 타이머 중지
    printf("  Address: 0x%02x",frame->address);
    printf("  Ack: %d\n\n",frame->seq);
    make_logMessage(frame, logMessage);  // 수신된 프레임 데이터를 로그 메시지로 변환
    writeLog(folderPath, "RX", logMessage);  // 로그 파일에 기록
    send_message("Rx", tx_frame.address, "", NULL, 0);  // 수신 메시지 전송
    free(p);  // 메모리 해제
    return NULL;
}

// 송신 콜백 함수 (마스터 간 통신)
void mtx(txData *tx) {
    LoRa_ctl *modem = (LoRa_ctl *)(tx->userPtr);
    make_logMessage(&tx_frame, logMessage);        // 로그 메시지 생성
    writeMasterLog("MTX", logMessage);  // 송신 로그 저장
    send_message("MTx", tx_frame.address, "", NULL, 0);
    if (is_broadcast == 0) {
        LoRa_receive(modem);
        set_timeout(6);
    } else {
        ack_receive = 1;
    }
}

// 수신 콜백 함수 (마스터 간 통신)
void *mrx(void *p) {
    rxData *rx = (rxData *)p;
    LoRa_ctl *modem = (LoRa_ctl *)(rx->userPtr);
    Frame *frame = (Frame *)(rx->buf);

    if ((!check_ID(frame->securityID)) || (!check_address(frame->address))) {
        free(p);
        return NULL;
    }

    ack_receive = 1;
    set_timeout(0);
    printf("  Address: 0x%02x", frame->address);
    printf("  Ack: %d\n\n", frame->seq);
    make_logMessage(frame, logMessage);
    writeMasterLog("MRX", logMessage);  // 수신 로그 저장
    free(p);
    return NULL;
}

////////////// 메인 함수 //////////////
int main() {
    pthread_create(&threads[0], NULL, nfc_tag, NULL);
    pthread_detach(threads[0]);

    set_securityID(MySecurityID);
    tx_frame.securityID[0] = MySecurityID[0];
    tx_frame.securityID[1] = MySecurityID[1];

    char txbuf[500];
    memset(txbuf, 0, sizeof(txbuf));

    modem.spiCS = 0;
    modem.tx.callback = mtx;  // 마스터 송신 콜백 설정
    modem.tx.data.buf = txbuf;
    modem.rx.callback = mrx;  // 마스터 수신 콜백 설정
    modem.rx.data.userPtr = (void *)(&modem);
    modem.tx.data.userPtr = (void *)(&modem);
    modem.tx.data.size = sizeof(Frame);
    modem.eth.preambleLen = 6;
    modem.eth.bw = BW125;
    modem.eth.sf = SF8;
    modem.eth.ecr = CR8;
    modem.eth.CRC = 1;
    modem.eth.freq = 915000000;
    modem.eth.resetGpioN = 4;
    modem.eth.dio0GpioN = 17;
    modem.eth.outPower = OP20;
    modem.eth.powerOutPin = PA_BOOST;
    modem.eth.AGC = 1;
    modem.eth.OCP = 240;
    modem.eth.implicitHeader = 0;
    modem.eth.syncWord = 0x12;

    LoRa_begin(&modem);

    int rc;
    mosquitto_lib_init();
    mosq = mosquitto_new("subscriber-client", true, NULL);
    if (!mosq) {
        fprintf(stderr, "Error: Out of memory.\n");
        return 1;
    }
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    rc = mosquitto_connect(mosq, "localhost", 1883, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Unable to connect: %s\n", mosquitto_strerror(rc));
        return 1;
    }
    mosquitto_loop_forever(mosq, -1, 1);

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();

    LoRa_end(&modem);
}
