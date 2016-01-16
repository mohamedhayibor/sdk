// Expectation for test: 
// main() {
//   var x = int.parse('1233');
//   var y = int.parse('1234');
//   print(x is num);
//   print(y is num);
//   print(x.compareTo(y));
//   print(x is num);
//   print(y is num);
// }

function() {
  var x = P.int_parse("1233", null, null), y = P.int_parse("1234", null, null), v0 = typeof x === "number", v1 = typeof y === "number";
  P.print(v0);
  P.print(v1);
  P.print(J.compareTo$1$ns(x, y));
  P.print(v0);
  P.print(v1);
}