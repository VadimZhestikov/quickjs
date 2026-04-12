var load = __loadScript;
var alert = print;
load('base.js');
load('earley-boyer.js');
try {
  var r1 = test(7);
  print("run1: " + r1);
} catch(e) { print("run1 error: " + e); }
try {
  var r2 = test(7);
  print("run2: " + r2);
} catch(e) { print("run2 error: " + e); }
