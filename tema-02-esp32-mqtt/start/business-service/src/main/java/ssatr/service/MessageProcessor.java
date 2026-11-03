package ssatr.service;

import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        while (!Thread.currentThread().isInterrupted()) {
            String message;
            try {
                message = inbox.take();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }

            // salvăm mesajul exact cum a sosit
            storage.append(kind, message);

            try {
                process(message);
            } catch (Exception e) {
                // un mesaj cu format invalid nu oprește thread-ul
                System.out.println("[" + kind + "] mesaj invalid ignorat: \"" + message
                        + "\" (" + e.getMessage() + ")");
            }
        }
    }

    private void process(String message) {
        String[] f = message.split(";");

        switch (kind) {
            case "telemetry": {
                // deviceId;timestamp;temperatura
                if (f.length != 3) throw new IllegalArgumentException("se așteptau 3 câmpuri");
                dashboard.onTelemetry(f[0], Double.parseDouble(f[2]));
                break;
            }
            case "alarm": {
                // deviceId;timestamp;temperatura;prag
                if (f.length != 4) throw new IllegalArgumentException("se așteptau 4 câmpuri");
                double temp = Double.parseDouble(f[2]);
                double threshold = Double.parseDouble(f[3]);
                dashboard.onAlarm(f[0], temp);
                System.out.println("!!! ALARMĂ " + f[0] + ": temperatura " + temp
                        + " a depășit pragul " + threshold);
                break;
            }
            case "status": {
                // deviceId;timestamp;text (textul poate conține spații)
                if (f.length < 3) throw new IllegalArgumentException("se așteptau 3 câmpuri");
                dashboard.onStatus(f[0]);
                System.out.println(">>> " + f[0] + " a răspuns: " + f[2]);
                break;
            }
            default:
                throw new IllegalArgumentException("tip necunoscut: " + kind);
        }
    }
}