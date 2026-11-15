// jshell script: run with  jshell -s /T/DEMO.JSH
int n = 6 * 7;
System.out.println("jshell says " + n);
String s = java.util.List.of("a", "b", "c").stream().collect(java.util.stream.Collectors.joining("+"));
System.out.println("joined " + s);
/exit
