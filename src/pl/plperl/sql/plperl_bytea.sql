
set plperl.pass_binary_bytea = true;

create function test_perl_bytea(str bytea) 
returns bytea
language plperl as
$func$

my $arg = shift;
my $enc = $arg;
$enc =~ s/(.)/sprintf("%.2x",ord($1))/egs;
elog(NOTICE,"encoded: $enc");
return $arg;

$func$;

select test_perl_bytea('\x01020304f1f2f3f4');
