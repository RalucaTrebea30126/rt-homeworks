package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 *
 * Evenimentele sosesc în coada alarmEvents (puse de taskul periodic).
 * Intervalul minim între sosiri este MIN_INTERARRIVAL_MS: între două eliberări
 * ale taskului (două alarme publicate) trebuie să treacă cel puțin atât.
 * Intervalul este impus de cod: evenimentele care sosesc mai des sunt
 * respinse, ca să nu fie inundat brokerul.
 */
public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<Double> alarmEvents) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        long minNanos = MIN_INTERARRIVAL_MS * 1_000_000L;
        long lastRelease = System.nanoTime() - minNanos;   // prima alarmă este acceptată

        while (!Thread.currentThread().isInterrupted()) {
            double t;
            try {
                t = alarmEvents.take();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }

            long now = System.nanoTime();
            if (now - lastRelease < minNanos) {
                System.out.println("[" + deviceId + "] alarma respinsă (interval minim între sosiri): "
                        + String.format(Locale.US, "%.2f", t));
                continue;
            }
            lastRelease = now;

            String payload = deviceId + ";" + System.currentTimeMillis() + ";"
                    + String.format(Locale.US, "%.2f", t) + ";"
                    + String.format(Locale.US, "%.2f", PeriodicTelemetryTask.currentThreshold());
            mqtt.publish(Config.alarmTopic(deviceId), payload);
            System.out.println("[" + deviceId + "] alarmă publicată: " + payload);
        }
    }
}