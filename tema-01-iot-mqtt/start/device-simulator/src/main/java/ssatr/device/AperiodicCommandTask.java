package ssatr.device;

import java.util.concurrent.BlockingQueue;

/**
 * TASK APERIODIC: execută comenzile primite de la business service.
 *
 * Comenzile sosesc oricând, pe MQTT, fără un interval minim între ele, și sunt
 * puse în coada commands. Fiecare comandă eliberează un job al taskului.
 * Comenzi suportate:
 *
 *   SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
 *   SET_THRESHOLD <valoare> schimbă pragul de alarmă
 *   RESET                   readuce senzorul la valoarea inițială
 *   STATUS                  raportează perioada și pragul curente
 *
 * După executarea fiecărei comenzi, device-ul publică pe Config.statusTopic(deviceId)
 * o confirmare: "deviceId;timestamp;text".
 */
public class AperiodicCommandTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<String> commands;
    private final PeriodicTelemetryTask periodic;

    public AperiodicCommandTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                BlockingQueue<String> commands, PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.commands = commands;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        while (!Thread.currentThread().isInterrupted()) {
            String command;
            try {
                command = commands.take();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }

            String reply;
            try {
                reply = execute(command.trim());
            } catch (Exception e) {
                // orice eroare (parametru lipsă, număr invalid etc.) nu oprește taskul
                reply = "ERROR " + command.trim() + " (" + e.getMessage() + ")";
            }

            mqtt.publish(Config.statusTopic(deviceId),
                    deviceId + ";" + System.currentTimeMillis() + ";" + reply);
            System.out.println("[" + deviceId + "] " + reply);
        }
    }

    private String execute(String command) {
        String[] p = command.split("\\s+");
        switch (p[0]) {
            case "SET_INTERVAL": {
                if (p.length != 2) throw new IllegalArgumentException("lipsește parametrul");
                long ms = Long.parseLong(p[1]);
                if (ms <= 0) throw new IllegalArgumentException("intervalul trebuie să fie > 0");
                periodic.setPeriodMs(ms);
                return "OK SET_INTERVAL " + ms;
            }
            case "SET_THRESHOLD": {
                if (p.length != 2) throw new IllegalArgumentException("lipsește parametrul");
                double v = Double.parseDouble(p[1]);
                periodic.setThreshold(v);
                return "OK SET_THRESHOLD " + p[1];
            }
            case "RESET":
                sensor.reset();
                return "OK RESET";
            case "STATUS":
                return "OK STATUS period=" + periodic.getPeriodMs()
                        + " threshold=" + periodic.getThreshold();
            default:
                throw new IllegalArgumentException("comandă necunoscută");
        }
    }
}