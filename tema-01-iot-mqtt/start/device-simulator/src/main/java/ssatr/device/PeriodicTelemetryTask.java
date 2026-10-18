package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * TASK PERIODIC, cu perioada T = periodMs: fiecare job citește senzorul și
 * publică valoarea pe topic-ul de telemetrie.
 *
 * Dacă temperatura depășește pragul, taskul NU tratează el alarma, ci doar
 * semnalează un eveniment în coada alarmEvents. Alarma este tratată de
 * taskul sporadic.
 */
public class PeriodicTelemetryTask implements Runnable {

    private static final double DEFAULT_THRESHOLD = 28.0;

    // pragul curent al procesului (un device simulat = un proces), citit și de taskul sporadic
    private static volatile double currentThreshold = DEFAULT_THRESHOLD;

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;

    // volatile: sunt citite aici, dar modificate de taskul aperiodic (alt thread)
    private volatile long periodMs = 2000;
    private volatile double threshold = DEFAULT_THRESHOLD;

    public PeriodicTelemetryTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                 BlockingQueue<Double> alarmEvents) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
        currentThreshold = DEFAULT_THRESHOLD;
    }

    @Override
    public void run() {
        long next = System.nanoTime();              // prima eliberare: acum

        while (!Thread.currentThread().isInterrupted()) {
            // --- jobul ---
            double t = sensor.read();
            String payload = deviceId + ";" + System.currentTimeMillis() + ";"
                    + String.format(Locale.US, "%.2f", t);
            mqtt.publish(Config.telemetryTopic(deviceId), payload);

            if (t > threshold) {
                alarmEvents.offer(t);               // offer(): nu blochează taskul periodic
            }

            // --- următoarea eliberare pe grilă (așteptare absolută) ---
            next += periodMs * 1_000_000L;          // periodMs se citește la fiecare job
            long wait = next - System.nanoTime();
            if (wait > 0) {
                try {
                    TimeUnit.NANOSECONDS.sleep(wait);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
            // dacă wait <= 0, jobul a depășit perioada: următorul pornește imediat
        }
    }

    public void setPeriodMs(long periodMs) {
        this.periodMs = periodMs;
    }

    public void setThreshold(double threshold) {
        this.threshold = threshold;
        currentThreshold = threshold;
    }

    public long getPeriodMs() {
        return periodMs;
    }

    public double getThreshold() {
        return threshold;
    }

    /** Pragul curent, pentru taskul sporadic (care nu are referință la acest task). */
    public static double currentThreshold() {
        return currentThreshold;
    }
}