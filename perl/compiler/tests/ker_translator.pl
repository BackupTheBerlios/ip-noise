#!/usr/bin/perl -w

use strict;

use IP::Noise::Text::Stream::In;
use IP::Noise::C::Parser;

use IP::Noise::C::Translator;
#use IP::Noise::Conn;
use IP::Noise::Conn::Ker;

open I, "<./tests/texts/parse_arbitrator/arbitrator1.txt";
my $stream = IP::Noise::Text::Stream::In->new(\*I);

my $arbitrator_ds;
eval {
    $arbitrator_ds = IP::Noise::C::Parser::parse_arbitrator($stream);
} ;

if ($@)
{
    my $e = $@;
        
    print $e->{'text'}, " at line ", 
          $e->{'line_num'}, ": \"", 
          $e->{'context'}, "\"\n";

    exit(-1);
}
 

close(I);

my $conn = IP::Noise::Conn::Ker->new();

my $translator = 
    IP::Noise::C::Translator->new(
        $arbitrator_ds, 
        $conn
        );

$translator->load_arbitrator();
