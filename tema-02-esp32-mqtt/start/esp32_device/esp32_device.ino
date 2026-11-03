// Device-ul IoT din tema 01, pe un ESP32-C3 Super Mini.
// Trei taskuri FreeRTOS: periodic, sporadic, aperiodic.
//
//   task periodic --(alarm_queue)--> task sporadic
//        |                                |
//        +--- telemetry ---> MQTT <-- alarm
//                              |
//                          commands --> command_received() --(command_queue)--> task aperiodic ---> status
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

#ifndef COMMAND_MAX_LEN
#define COMMAND_MAX_LEN 64
#endif

// Prioritățile taskurilor (între 2 și 19). Periodicul are cea mai mare,
// ca să-și respecte grila de eliberări.
#define PRIO_PERIODIC  5
#define PRIO_SPORADIC  4
#define PRIO_APERIODIC 3
#define TASK_STACK     4096

// Evenimentul din coada de alarme: se pune prin valoare (xQueueSend copiază).
struct AlarmEvent {
    float temperature;
    float threshold;
};

// Comanda din coada de comenzi: conține textul însuși, nu un pointer.
struct Command {
    char text[COMMAND_MAX_LEN];
};

static QueueHandle_t alarm_queue = NULL;
static QueueHandle_t command_queue = NULL;

// Modificate de taskul aperiodic, citite de cel periodic: volatile.
static volatile uint32_t period_ms = PERIOD_MS_INITIAL;
static volatile float threshold = THRESHOLD_INITIAL;

// Temperatura cipului, în °C (senzorul intern al ESP32-C3).
float read_temperature(void) {
    return temperatureRead();
}

// Publică pe topic-ul "status": "deviceId;timestamp;text"
static void publish_status(const char* text) {
    char msg[128];
    snprintf(msg, sizeof(msg), "%s;%lld;%s", DEVICE_ID, (long long)epoch_ms(), text);
    mqtt_publish("status", msg);
    Serial.printf("[aperiodic] %s\n", text);
}

// ---------------------------------------------------------------------------
// TODO 1: TASK PERIODIC
// ---------------------------------------------------------------------------
static void periodic_task(void* arg) {
    TickType_t last = xTaskGetTickCount();   // prima eliberare: acum

    for (;;) {
        // --- jobul ---
        float t = read_temperature();

        char msg[96];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f", DEVICE_ID, (long long)epoch_ms(), t);
        mqtt_publish("telemetry", msg);
        Serial.printf("[periodic] %s\n", msg);

        float th = threshold;
        if (t > th) {
            AlarmEvent ev = { t, th };
            // timeout 0: taskul periodic nu se blochează niciodată pe coadă
            if (xQueueSend(alarm_queue, &ev, 0) != pdTRUE) {
                Serial.println("[periodic] coada de alarme plină, eveniment pierdut");
            }
        }

        // --- așteptare absolută pe grila r_k = r_0 + kT ---
        xTaskDelayUntil(&last, pdMS_TO_TICKS(period_ms));
    }
}

// ---------------------------------------------------------------------------
// TODO 2: TASK SPORADIC
// ---------------------------------------------------------------------------
static void sporadic_task(void* arg) {
    uint32_t last_published = 0;
    bool has_published = false;              // prima alarmă este mereu acceptată

    for (;;) {
        AlarmEvent ev;
        xQueueReceive(alarm_queue, &ev, portMAX_DELAY);

        uint32_t now = millis();             // timp monoton
        if (has_published && (now - last_published) < MIN_INTERARRIVAL_MS) {
            Serial.printf("[sporadic] alarmă respinsă (interval minim între sosiri): %.2f\n",
                          ev.temperature);
            continue;
        }
        last_published = now;
        has_published = true;

        char msg[96];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f;%.2f",
                 DEVICE_ID, (long long)epoch_ms(), ev.temperature, ev.threshold);
        mqtt_publish("alarm", msg);
        Serial.printf("[sporadic] alarmă publicată: %s\n", msg);
    }
}

// ---------------------------------------------------------------------------
// TODO 3: TASK APERIODIC
// ---------------------------------------------------------------------------
static void execute_command(const char* text) {
    char name[24] = "";
    char arg[32] = "";
    char reply[96];

    int n = sscanf(text, "%23s %31s", name, arg);
    if (n < 1) {
        publish_status("ERROR comanda goala");
        return;
    }

    if (strcmp(name, "SET_INTERVAL") == 0) {
        char* end;
        long ms = (n == 2) ? strtol(arg, &end, 10) : 0;
        if (n != 2 || *end != '\0' || ms <= 0) {
            publish_status("ERROR SET_INTERVAL parametru invalid");
            return;
        }
        period_ms = (uint32_t)ms;
        snprintf(reply, sizeof(reply), "OK SET_INTERVAL %ld", ms);
        publish_status(reply);

    } else if (strcmp(name, "SET_THRESHOLD") == 0) {
        char* end;
        float v = (n == 2) ? strtof(arg, &end) : 0.0f;
        if (n != 2 || *end != '\0') {
            publish_status("ERROR SET_THRESHOLD parametru invalid");
            return;
        }
        threshold = v;
        snprintf(reply, sizeof(reply), "OK SET_THRESHOLD %s", arg);
        publish_status(reply);

    } else if (strcmp(name, "RESET") == 0) {
        period_ms = PERIOD_MS_INITIAL;
        threshold = THRESHOLD_INITIAL;
        publish_status("OK RESET");

    } else if (strcmp(name, "STATUS") == 0) {
        snprintf(reply, sizeof(reply), "OK STATUS period=%lu threshold=%.2f",
                 (unsigned long)period_ms, (double)threshold);
        publish_status(reply);

    } else {
        snprintf(reply, sizeof(reply), "ERROR comanda necunoscuta %s", name);
        publish_status(reply);
    }
}

static void aperiodic_task(void* arg) {
    for (;;) {
        Command cmd;
        xQueueReceive(command_queue, &cmd, portMAX_DELAY);
        execute_command(cmd.text);           // o comandă greșită nu oprește taskul
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (pe taskul MQTT).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);

    Command cmd;
    strncpy(cmd.text, text, COMMAND_MAX_LEN - 1);
    cmd.text[COMMAND_MAX_LEN - 1] = '\0';

    // timeout 0: nu blocăm taskul clientului MQTT
    if (xQueueSend(command_queue, &cmd, 0) != pdTRUE) {
        Serial.println("[mqtt] coada de comenzi plină, comandă pierdută");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);
    delay(200);
    Serial.println();
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): cozile, create înainte de mqtt_start()
    alarm_queue = xQueueCreate(8, sizeof(AlarmEvent));
    command_queue = xQueueCreate(8, sizeof(Command));
    if (alarm_queue == NULL || command_queue == NULL) {
        Serial.println("Eroare: nu s-au putut crea cozile");
        for (;;) delay(1000);
    }

    mqtt_start();

    // TODO 0 (b): taskurile, pornite după mqtt_start()
    xTaskCreate(periodic_task,  "periodic",  TASK_STACK, NULL, PRIO_PERIODIC,  NULL);
    xTaskCreate(sporadic_task,  "sporadic",  TASK_STACK, NULL, PRIO_SPORADIC,  NULL);
    xTaskCreate(aperiodic_task, "aperiodic", TASK_STACK, NULL, PRIO_APERIODIC, NULL);
}

void loop() {
    // Totul se face în taskuri.
    delay(1000);
}