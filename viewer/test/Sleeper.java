public class Sleeper {
    public static void main(String[] args) throws Exception {
        System.out.println("Sleeper JVM up");
        while (true) { Thread.sleep(1000); }
    }
}
