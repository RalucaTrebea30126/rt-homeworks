package ssatr.service;

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme etc.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci accesul trebuie sincronizat.
 */
public class Dashboard {

    /** Starea unui device. */
    private static class DeviceState {
        double lastTemperature = Double.NaN;
        long lastMessageAt;      // momentul primirii ultimului mesaj (System.currentTimeMillis)
        int alarmCount;
    }

    // TreeMap: device-urile apar ordonate după id. Accesul este sincronizat pe this.
    private final Map<String, DeviceState> devices = new TreeMap<>();

    private DeviceState stateOf(String deviceId) {
        return devices.computeIfAbsent(deviceId, id -> new DeviceState());
    }

    public synchronized void onTelemetry(String deviceId, double temperature) {
        DeviceState s = stateOf(deviceId);
        s.lastTemperature = temperature;
        s.lastMessageAt = System.currentTimeMillis();
    }

    public synchronized void onAlarm(String deviceId, double temperature) {
        DeviceState s = stateOf(deviceId);
        s.lastTemperature = temperature;
        s.lastMessageAt = System.currentTimeMillis();
        s.alarmCount++;
    }

    public synchronized void onStatus(String deviceId) {
        stateOf(deviceId).lastMessageAt = System.currentTimeMillis();
    }

    /** Afișează un tabel cu starea tuturor device-urilor. */
    public synchronized void print() {
        if (devices.isEmpty()) {
            System.out.println("(niciun device nu a trimis încă mesaje)");
            return;
        }

        long now = System.currentTimeMillis();
        System.out.println(String.format("%-12s %-8s %-16s %s", "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME"));
        for (Map.Entry<String, DeviceState> e : devices.entrySet()) {
            DeviceState s = e.getValue();
            String temp = Double.isNaN(s.lastTemperature)
                    ? "-" : String.format(Locale.US, "%.2f", s.lastTemperature);
            String age = "acum " + (now - s.lastMessageAt) / 1000 + " s";
            System.out.println(String.format("%-12s %-8s %-16s %d", e.getKey(), temp, age, s.alarmCount));
        }
    }
}