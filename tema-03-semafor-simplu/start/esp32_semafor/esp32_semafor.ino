// Semafor pentru pietoni, cu buton, pe ESP32-C3 Super Mini.
//
// Placa este controlerul semaforului; lămpile sunt desenate de dashboard, care
// le aprinde după mesajele "lights" publicate de placă. Patru activități:
//
//   ISR buton BOOT ----+
//                      +--(coada butonului)--> task sporadic
//   command_received --+   (BUTTON)                 |
//                                                   v
//                                         [ starea semaforului ] <-- mutex
//                                            ^          |
//                                            |          v
//                         task periodic de control   task periodic de ieșire
//                         (fazele, T = 100 ms)       (lămpile, T = 500 ms) --> lights
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Fazele semaforului. Numele din PHASE_NAMES sunt cele din mesajul "lights".
enum Phase { CAR_GREEN, CAR_YELLOW, ALL_RED, PED_GREEN };
const char* const PHASE_NAMES[] = {"CAR_GREEN", "CAR_YELLOW", "ALL_RED", "PED_GREEN"};

// Culorile unei lămpi, cu numele din mesajul "lights".
enum Light { RED, YELLOW, GREEN };
const char* const LIGHT_NAMES[] = {"RED", "YELLOW", "GREEN"};

// Priorități (2..19). Taskurile periodice după Rate Monotonic: perioada mai
// mică (control, 100 ms) => prioritate mai mare decât ieșirea (500 ms).
// Taskul sporadic face muncă foarte scurtă și rulează rar.
#define PRIO_CONTROL  6
#define PRIO_OUTPUT   5
#define PRIO_SPORADIC 4
#define TASK_STACK    4096

// ---------------------------------------------------------------------------
// TODO 0 (c): STAREA SEMAFORULUI, partajată de taskuri și protejată de mutex
// ---------------------------------------------------------------------------
struct SemaphoreState {
    Phase    phase;            // faza curentă
    uint32_t phase_entered_ms; // momentul intrării în fază (millis(), monoton)
    bool     ped_request;      // cererea pietonului (lampa AȘTEPTAȚI)
};

static SemaphoreState state;
static SemaphoreHandle_t state_mutex = NULL;

// Elementul cozii butonului: conține momentul sosirii, pus de cine trimite
// (ISR sau command_received), nu citit de task când preia apăsarea.
struct ButtonPress {
    uint32_t arrival_ms;
};

static QueueHandle_t button_queue = NULL;

// Moment monoton în ms, din tick-uri (folosit și din ISR, cu varianta FromISR).
static inline uint32_t ticks_to_ms(TickType_t ticks) {
    return (uint32_t)ticks * portTICK_PERIOD_MS;
}

// ---------------------------------------------------------------------------
// TODO 1: TASK PERIODIC DE CONTROL (T = CONTROL_PERIOD_MS)
// ---------------------------------------------------------------------------
static void control_task(void* arg) {
    TickType_t last = xTaskGetTickCount();   // prima eliberare: acum

    for (;;) {
        bool changed = false;
        Phase new_phase = CAR_GREEN;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        uint32_t now = millis();
        uint32_t elapsed = now - state.phase_entered_ms;   // cât durează faza

        switch (state.phase) {
            case CAR_GREEN:
                // se termină doar dacă a trecut verdele minim ȘI există cerere
                if (elapsed >= CAR_GREEN_MIN_MS && state.ped_request) {
                    new_phase = CAR_YELLOW; changed = true;
                }
                break;
            case CAR_YELLOW:
                if (elapsed >= CAR_YELLOW_MS) { new_phase = ALL_RED; changed = true; }
                break;
            case ALL_RED:
                if (elapsed >= ALL_RED_MS) {
                    new_phase = PED_GREEN; changed = true;
                    state.ped_request = false;   // pietonul primește ce a cerut
                }
                break;
            case PED_GREEN:
                if (elapsed >= PED_GREEN_MS) { new_phase = CAR_GREEN; changed = true; }
                break;
        }

        if (changed) {
            state.phase = new_phase;
            state.phase_entered_ms = now;
        }
        xSemaphoreGive(state_mutex);

        // Serial doar după ce mutexul a fost eliberat
        if (changed) {
            Serial.printf("[control] faza nouă: %s\n", PHASE_NAMES[new_phase]);
        }

        // așteptare absolută: grila r_k = r_0 + kT, fără derivă
        xTaskDelayUntil(&last, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

// ---------------------------------------------------------------------------
// TODO 2: TASK PERIODIC DE IEȘIRE (T = OUTPUT_PERIOD_MS)
// ---------------------------------------------------------------------------
static void output_task(void* arg) {
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        // copiem starea cu mutexul luat, cât mai scurt
        SemaphoreState s;
        uint32_t now;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        s = state;
        now = millis();
        xSemaphoreGive(state_mutex);

        uint32_t elapsed = now - s.phase_entered_ms;

        // culorile lămpilor, din faza copiată
        Light cars, peds;
        switch (s.phase) {
            case CAR_GREEN:  cars = GREEN;  peds = RED;   break;
            case CAR_YELLOW: cars = YELLOW; peds = RED;   break;
            case ALL_RED:    cars = RED;    peds = RED;   break;
            default:         cars = RED;    peds = GREEN; break;   // PED_GREEN
        }

        // secundele rămase din verdele pietonilor, rotunjite în sus
        int seconds = 0;
        if (s.phase == PED_GREEN && elapsed < PED_GREEN_MS) {
            uint32_t remaining_ms = PED_GREEN_MS - elapsed;
            seconds = (int)((remaining_ms + 999) / 1000);
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "%s;%lld;%s;%s;%s;%d;%d",
                 DEVICE_ID, (long long)epoch_ms(),
                 PHASE_NAMES[s.phase], LIGHT_NAMES[cars], LIGHT_NAMES[peds],
                 s.ped_request ? 1 : 0, seconds);
        mqtt_publish("lights", msg);

        // LED-ul plăcii: aprins pe LOW cât timp lampa AȘTEPTAȚI e aprinsă
        digitalWrite(LED_PIN, s.ped_request ? LOW : HIGH);

        xTaskDelayUntil(&last, pdMS_TO_TICKS(OUTPUT_PERIOD_MS));
    }
}

// ---------------------------------------------------------------------------
// TODO 3: RUTINA DE ÎNTRERUPERE a butonului BOOT (scurtă, doar FromISR)
// ---------------------------------------------------------------------------
void IRAM_ATTR button_isr(void) {
    ButtonPress press = { ticks_to_ms(xTaskGetTickCountFromISR()) };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(button_queue, &press, &woken);
    portYIELD_FROM_ISR(woken);   // dacă a fost trezit un task mai prioritar, rulează imediat
}

// ---------------------------------------------------------------------------
// TODO 3: TASK SPORADIC (apăsările butonului)
// ---------------------------------------------------------------------------
static void button_task(void* arg) {
    uint32_t last_accepted_ms = 0;
    bool has_accepted = false;   // prima apăsare este mereu acceptată

    for (;;) {
        ButtonPress press;
        xQueueReceive(button_queue, &press, portMAX_DELAY);

        // intervalul minim între sosiri, măsurat pe momentul sosirii apăsării
        if (has_accepted && (press.arrival_ms - last_accepted_ms) < MIN_INTERARRIVAL_MS) {
            Serial.println("[buton] apăsare respinsă (interval minim între sosiri)");
            continue;
        }
        last_accepted_ms = press.arrival_ms;
        has_accepted = true;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        state.ped_request = true;    // mai multe apăsări = tot o singură cerere
        xSemaphoreGive(state_mutex);

        Serial.println("[buton] cerere pietoni înregistrată");
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);

    // doar "BUTTON" contează; celelalte comenzi se ignoră
    if (strcmp(text, "BUTTON") == 0) {
        ButtonPress press = { ticks_to_ms(xTaskGetTickCount()) };
        // timeout 0: nu blocăm taskul clientului MQTT
        if (xQueueSend(button_queue, &press, 0) != pdTRUE) {
            Serial.println("[mqtt] coada butonului plină, apăsare pierdută");
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);
    delay(200);
    Serial.println();
    Serial.printf("=== tema 03: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    wifi_connect();
    time_sync();

    // TODO 0 (a): mutexul și coada, create înainte de mqtt_start()
    state_mutex = xSemaphoreCreateMutex();
    button_queue = xQueueCreate(16, sizeof(ButtonPress));
    if (state_mutex == NULL || button_queue == NULL) {
        Serial.println("Eroare: nu s-au putut crea mutexul sau coada");
        for (;;) delay(1000);
    }

    // starea inițială: CAR_GREEN, fără cerere, momentul intrării = acum
    state.phase = CAR_GREEN;
    state.phase_entered_ms = millis();
    state.ped_request = false;

    mqtt_start();

    // TODO 0 (b): taskurile, apoi întreruperea butonului
    xTaskCreate(control_task, "control", TASK_STACK, NULL, PRIO_CONTROL,  NULL);
    xTaskCreate(output_task,  "output",  TASK_STACK, NULL, PRIO_OUTPUT,   NULL);
    xTaskCreate(button_task,  "button",  TASK_STACK, NULL, PRIO_SPORADIC, NULL);

    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}