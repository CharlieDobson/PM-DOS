# mkcd.pl - build the discs the CD-ROM code has to be TESTED against,
#           because no tool on this machine can make them.
#
# MKISO.PS1 builds a Joliet disc out of IMAPI2FS, which is Windows' own
# image writer and is the right tool for that job.  It cannot make any
# of the three discs this release needs:
#
#   ROCK RIDGE      IMAPI2FS has never written it - it is the Unix
#                   world's answer to the same problem Joliet solves,
#                   and Windows solved it its own way.
#   MULTI-SESSION   a second session is a second TRACK at a high LBA
#                   whose directory records point BACKWARDS into the
#                   first one.  That is a property of the DISC, not of
#                   a file, so no .iso writer can express it.
#   CD AUDIO        an audio track is not a filesystem at all.
#
# So this writes ISO9660 itself, and writes .CUE sheets beside the
# tracks so 86Box can mount a disc with more than one thing on it.
#
# WHAT MAKES THE MULTI-SESSION DISC A REAL TEST and not a rehearsal:
# session two's root directory lists a file whose extent is INSIDE
# SESSION ONE.  Reading it proves the one fact the kernel's new code
# rests on - that extents in a later session are absolute from the
# start of the disc - and a reader that quietly made them relative to
# the session would produce garbage rather than an error.  Session two
# also DROPS a file session one had, so "which session am I looking
# at" is answerable from a DIR listing alone.
#
# THE ORACLE IS NOT THIS SCRIPT.  Windows ships bsdtar (libarchive) as
# tar.exe, and libarchive reads Rock Ridge - including the name
# continuations and the deep-directory relocation this builds on
# purpose.  "tar -tvf" over the image is therefore an independent
# reading of it, which is the same discipline TOOLS\ISODUMP.PL exists
# for on the ISO9660 side.
#
# Usage:  perl MKCD.PL <what> [outdir]
#           rr      RRTEST.ISO    - Rock Ridge, and NO Joliet, so the
#                                   kernel has to read the real names
#                                   out of the SUSP fields or show the
#                                   mangled 8.3 ones
#           multi   MSESS.CUE     - two data tracks, two sessions
#           extra   CDEXTRA.CUE   - audio first, data second: the disc
#                                   that does not mount at all today
#           mixed   MIXED.CUE     - data first, audio after: what a
#                                   game with CD music actually is
#           all     all four
#         outdir defaults to S:\Software\PMDOSCD
use strict;
use warnings;

my $BLK = 2048;                 # a logical block
my $RAW = 2352;                 # an audio sector
our $RELOC = 8;                 # relocate a directory whose children
                                # would be deeper than this many levels.
                                # Eight is what ISO9660 allows and what a
                                # real writer uses; a test disc lowers it,
                                # for the reason disc_rrdeep explains
my $GAP = 11400;                # lead-out + lead-in + pregap between
                                # two sessions, which is what a real
                                # recorder leaves and what makes the
                                # second session start at a number
                                # nothing could arrive at by accident

#---------------------------------------------------------------------
# The tree, as a handful of nested hashes.  F() is a file with its own
# text in it - every file says what it is, so a TYPE on the guest is a
# test and not just a read.
#---------------------------------------------------------------------
sub F { my ($n,$t) = @_; return { name=>$n, dir=>0, data=>$t, kids=>[] } }
sub D { my ($n,@k)  = @_; return { name=>$n, dir=>1, kids=>[@k] } }

# FS is a file written as SEVERAL EXTENTS - several directory records
# in a row with one name between them, which is what ISO9660 does with
# a file too big for the 32-bit size field.  The real article is over
# four gigabytes; this is the same structure at a size a test disc can
# hold, which is the only way the reader's handling of it can be
# exercised on this machine.
sub FS { my ($n,$t,$pieces) = @_; my $f = F($n,$t); $f->{split} = $pieces;
         return $f }

# chunk - $n bytes that say what they are on the first line.  The rest
# is dots: a file whose pieces are all identical would not show which
# of them had been read.
sub chunk {
  my ($label, $n) = @_;
  my $line = ('.' x 70) . "\r\n";
  my $s = "$label\r\n";
  $s .= $line while length($s) + length($line) <= $n;
  $s .= '.' x ($n - length($s) - 2) . "\r\n" if length($s) < $n;
  return substr($s, 0, $n);
}
sub splitdata {
  return chunk('PIECE ONE of three, and the first extent of this file.', 2048)
       . chunk('PIECE TWO, the second extent.  A reader that stops at the'
             . ' first shows 2048 bytes.', 2048)
       . "PIECE THREE - the last extent, and the last line of the file.\r\n";
}

#=====================================================================
# ISO9660 primitives.  Every multi-byte number is stored twice, little
# endian and then big, which is the format's one unmistakable feature.
#=====================================================================
sub both32 { return pack('V', $_[0]) . pack('N', $_[0]) }
sub both16 { return pack('v', $_[0]) . pack('n', $_[0]) }
sub blocks { my $n = shift; return int(($n + $BLK - 1) / $BLK) }
sub pad    { my ($s,$n,$c) = @_; $c = ' ' unless defined $c;
             return substr($s . ($c x $n), 0, $n) }

# The seven-byte recording time on a directory record: binary, and the
# year counted from 1900.
sub dirdate {
  return pack('C7', 126, 9, 11, 12, 0, 0, 0);   # 2026-09-11 12:00 UTC
}
# ...and the seventeen-character one on a volume descriptor, which is
# the same instant spelled as text for no reason anybody remembers.
sub voldate { return '2026091112000000' . chr(0) }

#=====================================================================
# iso_names - give everything an ISO9660 name, the way a real writer
#             would have to: upper case, d-characters only, 8.3, and
#             unique within its directory.
#
# THIS IS THE POINT OF THE ROCK RIDGE DISC.  A reader with no Rock
# Ridge shows exactly these names, so the test is not "are there
# names" but "which of the two sets came back".
#=====================================================================
sub iso_names {
  my ($dir) = @_;
  my %used;
  for my $k (@{$dir->{kids}}) {
    my ($base, $ext) = ($k->{name}, '');
    if (!$k->{dir} and $base =~ /^(.*)\.([^.]*)$/) { ($base, $ext) = ($1, $2) }
    for ($base, $ext) { s/[^A-Za-z0-9_]/_/g; $_ = uc $_ }
    $base = '_' if $base eq '';
    $base = substr($base, 0, 8);
    $ext  = substr($ext,  0, 3);
    my $try = $ext ne '' ? "$base.$ext" : $base;
    my $n = 0;
    while ($used{$try}++) {                     # collide: number the tail
      $n++;
      my $stem = substr($base, 0, 8 - length($n));
      $try = $ext ne '' ? "$stem$n.$ext" : "$stem$n";
    }
    $k->{iso} = $try;
    $k->{id}  = $k->{dir} ? $try : "$try;1";    # a file carries a version
    iso_names($k) if $k->{dir};
  }
}

#=====================================================================
# relocate - ISO9660 allows EIGHT levels of directory and no more, so
#            a deeper tree cannot be expressed at all.  Rock Ridge's
#            answer is to MOVE the too-deep directory to a holding pen
#            in the root and leave three markers behind:
#
#   CL  in the parent, on a record that looks like a FILE, saying
#       "the directory you wanted is really over there"
#   RE  on the moved directory itself, saying "do not show me here"
#   PL  on the moved directory's "..", saying where its parent really
#       is - without which CD .. from inside it walks to the pen
#
# A reader that ignores all three shows a truncated tree and a
# directory full of strange zero-length files, which is exactly what
# makes this worth putting on the test disc.
#=====================================================================
sub relocate {
  my ($root) = @_;
  my $pen = D('rr_moved');
  my @moved;
  my $walk;
  $walk = sub {
    my ($dir, $depth) = @_;
    my @keep;
    for my $k (@{$dir->{kids}}) {
      if ($k->{dir} and $depth >= $RELOC) {     # this one would be too deep
        push @moved, $k;
        $k->{re} = 1;                           # "I am not really here"
        $k->{plparent} = $dir;                  # ...and this is where I was
        push @keep, { name=>$k->{name}, dir=>0, kids=>[], data=>'',
                      cl=>$k, iso=>$k->{iso}, id=>$k->{iso} };
        next;
      }
      $walk->($k, $depth + 1) if $k->{dir};
      push @keep, $k;
    }
    $dir->{kids} = \@keep;
  };
  $walk->($root, 1);
  return unless @moved;
  $pen->{kids} = \@moved;
  $pen->{iso} = 'RR_MOVED'; $pen->{id} = 'RR_MOVED';
  push @{$root->{kids}}, $pen;
}

#=====================================================================
# The SUSP entries.  Each is a signature, its own length, a version
# byte and then its payload - a format deliberately shaped so that a
# reader can walk past anything it has never heard of.
#=====================================================================
sub susp_sp { return 'SP' . pack('CCCCC', 7, 1, 0xBE, 0xEF, 0) }
sub susp_re { return 'RE' . pack('CC', 4, 1) }
sub susp_cl { return 'CL' . pack('CC', 12, 1) . both32($_[0]) }
sub susp_pl { return 'PL' . pack('CC', 12, 1) . both32($_[0]) }
sub susp_px {
  my ($flags) = @_;
  my $mode = $flags ? 040755 : 0100644;
  return 'PX' . pack('CC', 36, 1)
       . both32($mode) . both32(1) . both32(0) . both32(0);
}
sub susp_ce { return 'CE' . pack('CC', 28, 1)
                   . both32($_[0]) . both32($_[1]) . both32($_[2]) }
sub susp_er {
  my $id  = 'RRIP_1991A';
  my $des = 'THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR '
          . 'POSIX FILE SYSTEM SEMANTICS';
  my $src = 'PLEASE CONTACT DISC PUBLISHER FOR SPECIFICATION SOURCE.';
  return 'ER' . pack('CCCCCC', 8 + length($id) + length($des) + length($src),
                     1, length($id), length($des), length($src), 1)
       . $id . $des . $src;
}
# NM is the name itself, and a long one does not fit in one entry: the
# CONTINUE flag on all but the last is how the pieces are joined.
sub susp_nm {
  my ($name) = @_;
  my @out;
  my @parts;
  while (length($name) > 200) {
    push @parts, substr($name, 0, 200);
    $name = substr($name, 200);
  }
  push @parts, $name;
  for my $i (0 .. $#parts) {
    my $flags = ($i < $#parts) ? 1 : 0;         # bit 0 = CONTINUE
    push @out, 'NM' . pack('CCC', 5 + length($parts[$i]), 1, $flags)
             . $parts[$i];
  }
  return @out;
}

#=====================================================================
# plan_rec - how long one directory record is, and which of its system
#            use entries have to go in a continuation area.
#
# A record may not exceed 255 bytes, and the identifier is at the
# front of it, so a long name plus its Rock Ridge baggage runs out of
# room in exactly the cases Rock Ridge exists for.  What is left over
# goes in a CE area, and the CE entry that names it is itself 28 bytes
# - so the split has to leave room for the pointer as well as for the
# thing it points at.
#=====================================================================
sub plan_rec {
  my ($id, $entries) = @_;
  my $fixed = 33 + length($id) + ((length($id) % 2) ? 0 : 1);
  my (@in, @ce);
  my $len = $fixed;
  for my $e (@$entries) {
    if (@ce or $len + length($e) > 255 - 28) { push @ce, $e; next }
    push @in, $e;
    $len += length($e);
  }
  my $celen = 0;
  $celen += length($_) for @ce;
  $len += 28 if @ce;
  $len++ if $len % 2;                           # records are even
  die "record for $id is $len bytes\n" if $len > 255;
  return { id=>$id, fixed=>$fixed, len=>$len, in=>\@in, ce=>\@ce,
           celen=>$celen };
}

#=====================================================================
# plan_dir - lay one directory out as a list of records, in the order
#            and the sizes they will be written.
#
# A RECORD NEVER CROSSES A BLOCK.  The tail of a block is padded with
# zeros and the next record starts at the top of the next one, which
# is the single thing about the format a walker written for FAT gets
# wrong - and therefore the thing the writer has to get right for the
# test to mean anything.
#=====================================================================
sub plan_dir {
  my ($dir, $rr) = @_;
  my @recs;

  # "." and ".." come first and are one byte of identifier each.
  # SP MUST BE THE FIRST SYSTEM USE ENTRY OF THE ROOT'S "." RECORD,
  # and that is not a formality: it is the ONLY thing that tells a
  # reader the disc uses SUSP at all, so an SP written second is an SP
  # that is never found.  This builder emitted PX first to begin with
  # and bsdtar duly listed the mangled 8.3 names - the oracle earning
  # its keep on the writer before the kernel had read a byte.
  my @dot = ();
  push @dot, susp_sp() if $rr and $dir->{isroot};
  push @dot, susp_px(1) if $rr;
  push @dot, susp_er() if $rr and $dir->{isroot};
  my @dotdot = $rr ? (susp_px(1)) : ();
  push @dotdot, susp_pl(0) if $rr and $dir->{plparent};  # value filled later
  push @recs, plan_rec(chr(0), \@dot);
  $recs[-1]{self} = $dir;
  push @recs, plan_rec(chr(1), \@dotdot);
  $recs[-1]{parent} = $dir;

  for my $k (sort { $a->{id} cmp $b->{id} } @{$dir->{kids}}) {
    my @e;
    if ($rr) {
      push @e, susp_px($k->{dir});
      push @e, susp_re() if $k->{re};
      push @e, susp_cl(0) if $k->{cl};          # value filled later
      push @e, susp_nm($k->{name});
    }
    # A FILE IN SEVERAL PIECES IS SEVERAL RECORDS IN A ROW, all with the
    # same name and all but the last carrying the "not the last extent"
    # flag.  That is what the format says a file too big for one extent
    # looks like, and writing it here is the only way to have a disc
    # that tests a reader's handling of it - the real article is over
    # four gigabytes and nothing on this machine would build one.
    my $pieces = $k->{split} || 1;
    for my $i (0 .. $pieces - 1) {
      my $r = plan_rec($k->{id}, [@e]);
      $r->{node} = $k;
      $r->{piece} = $i if $k->{split};
      push @recs, $r;
    }
  }

  # pack them into blocks, and remember where each one lands
  my $off = 0;
  for my $r (@recs) {
    my $room = $BLK - ($off % $BLK);
    $off += $room if $r->{len} > $room;         # it will not fit: next block
    $r->{off} = $off;
    $off += $r->{len};
  }
  $dir->{recs} = \@recs;
  $dir->{len}  = blocks($off) * $BLK;
  my $ce = 0;
  for my $r (@recs) { next unless $r->{celen};
                      $r->{ceoff} = $ce; $ce += $r->{celen} }
  $dir->{celen} = $ce;
  die "continuation area for a directory exceeds one block\n" if $ce > $BLK;
}

#=====================================================================
# emit_rec - the bytes of one directory record.
#=====================================================================
sub emit_rec {
  my ($r, $extent, $size, $flags, $ceblk) = @_;
  my $id = $r->{id};
  my $b = pack('C', $r->{len}) . pack('C', 0)
        . both32($extent) . both32($size) . dirdate()
        . pack('C', $flags) . pack('CC', 0, 0) . both16(1)
        . pack('C', length($id)) . $id;
  $b .= chr(0) unless length($id) % 2;          # pad to an even boundary
  $b .= $_ for @{$r->{in}};
  $b .= susp_ce($ceblk, $r->{ceoff}, $r->{celen}) if @{$r->{ce}};
  $b .= chr(0) while length($b) < $r->{len};
  return $b;
}

#=====================================================================
# build - the whole image, as a string of blocks.
#
#   root    the tree
#   volid   the volume name
#   rr      write Rock Ridge
#   base    the LBA THIS TRACK STARTS AT, which is what makes a second
#           session possible: every extent written is absolute from
#           the start of the DISC, and only the place the descriptors
#           are looked for moves.
#
# Files marked "carried" are not written at all - they already exist,
# in an earlier session, and the record merely points at them.
#=====================================================================
sub build {
  my (%o) = @_;
  my $root = $o{root};
  my $base = $o{base} || 0;
  my $rr   = $o{rr}   || 0;
  $root->{isroot} = 1;
  $root->{iso} = ''; $root->{id} = '';

  iso_names($root);
  relocate($root) if $rr;

  # every directory, breadth first, which is also the order the path
  # table wants them in
  my @dirs;
  my @queue = ($root);
  $root->{pathno} = 1; $root->{parentno} = 1;
  while (@queue) {
    my $d = shift @queue;
    push @dirs, $d;
    for my $k (@{$d->{kids}}) {
      next unless $k->{dir};
      $k->{pathno} = scalar(@dirs) + scalar(@queue) + 1;
      $k->{parentno} = $d->{pathno};
      push @queue, $k;
    }
  }
  plan_dir($_, $rr) for @dirs;
  # ...and who each directory's parent is, which the ".." records below
  # need and which the tree itself does not record
  for my $d (@dirs) { for my $k (@{$d->{kids}}) { $k->{up} = $d if $k->{dir} } }

  # the path table, which nothing in PM-DOS reads and every other
  # reader might, so it is written properly rather than plausibly
  my $ptlen = 0;
  for my $d (@dirs) {
    my $id = $d->{isroot} ? chr(0) : $d->{iso};
    $ptlen += 8 + length($id) + (length($id) % 2);
  }

  # ---- addresses ----
  my $lba = $base + 16;
  my $pvdblk  = $lba++;
  my $termblk = $lba++;
  my $ptl = $lba; $lba += blocks($ptlen);
  my $ptm = $lba; $lba += blocks($ptlen);
  for my $d (@dirs) {
    $d->{extent} = $lba; $lba += blocks($d->{len});
    if ($d->{celen}) { $d->{ceblk} = $lba; $lba += blocks($d->{celen}) }
  }
  my @files;
  my $collect;
  $collect = sub {
    my ($d) = @_;
    for my $k (@{$d->{kids}}) {
      if ($k->{dir}) { $collect->($k); next }
      next if $k->{cl};                         # a marker, not a file
      next if $k->{carried};                    # it is in another session
      $k->{size} = length($k->{data});
      $k->{extent} = $lba;
      if ($k->{split}) {
        # THE PIECES ARE CONTIGUOUS AND ALL BUT THE LAST ARE WHOLE
        # BLOCKS, because ISO9660 says an extent that is not the last
        # one of a file occupies whole blocks - a file cannot restart
        # in the middle of a sector.
        my $n = $k->{split};
        my $whole = int(blocks($k->{size}) / $n) || 1;
        my (@sz, @ex);
        my $left = $k->{size};
        for my $i (0 .. $n - 2) {
          my $take = $whole * $BLK;
          $take = $left if $take > $left;
          push @sz, $take;
          push @ex, $lba;
          $lba += blocks($take) || 1;
          $left -= $take;
        }
        push @sz, $left;
        push @ex, $lba;
        $lba += blocks($left) || 1;
        $k->{sizes} = \@sz;
        $k->{extents} = \@ex;
      } else {
        $lba += blocks($k->{size}) || 1;
      }
      push @files, $k;
    }
  };
  $collect->($root);
  my $total = $lba;                             # one past the last block used

  # ---- the image ----
  my %img;                                      # absolute block -> bytes
  my $put = sub { my ($blk, $bytes) = @_;
                  while (length($bytes)) {
                    $img{$blk++} = pad(substr($bytes, 0, $BLK), $BLK, chr(0));
                    $bytes = length($bytes) > $BLK ? substr($bytes, $BLK) : '';
                  } };

  # directories
  for my $d (@dirs) {
    my $bytes = chr(0) x $d->{len};
    my $ce = '';
    for my $r (@{$d->{recs}}) {
      my ($ext, $size, $flags);
      if ($r->{self})        { ($ext,$size,$flags) = ($d->{extent}, $d->{len}, 2) }
      elsif ($r->{parent})   {
        # ".." IS THE ISO PARENT, which for a relocated directory is
        # the holding pen and not where the directory belongs.  That
        # is precisely why PL exists, and writing the real parent here
        # instead would make the marker meaningless.
        my $p = $d->{up} || $root;
        ($ext,$size,$flags) = ($p->{extent}, $p->{len}, 2);
        # PL names the REAL parent of a relocated directory
        if ($d->{plparent}) {
          for my $e (@{$r->{in}}, @{$r->{ce}}) {
            substr($e, 4, 8) = both32($d->{plparent}{extent})
              if substr($e, 0, 2) eq 'PL';
          }
        }
      }
      else {
        my $k = $r->{node};
        if ($k->{cl}) {
          ($ext,$size,$flags) = ($k->{cl}{extent}, $k->{cl}{len}, 0);
          for my $e (@{$r->{in}}, @{$r->{ce}}) {
            substr($e, 4, 8) = both32($k->{cl}{extent})
              if substr($e, 0, 2) eq 'CL';
          }
        }
        elsif ($k->{dir}) { ($ext,$size,$flags) = ($k->{extent}, $k->{len}, 2) }
        elsif (defined $r->{piece}) {
          my $i = $r->{piece};
          ($ext,$size) = ($k->{extents}[$i], $k->{sizes}[$i]);
          $flags = ($i < $k->{split} - 1) ? 0x80 : 0;   # more to come
        }
        else              { ($ext,$size,$flags) = ($k->{extent}, $k->{size}, 0) }
      }
      substr($bytes, $r->{off}, $r->{len}) = emit_rec($r, $ext, $size, $flags,
                                                      $d->{ceblk} || 0);
      if (@{$r->{ce}}) { $ce .= $_ for @{$r->{ce}} }
    }
    $put->($d->{extent}, $bytes);
    $put->($d->{ceblk}, $ce) if $d->{celen};
  }

  # the parent links the loop above wanted
  for my $d (@dirs) { for my $k (@{$d->{kids}}) { $k->{up} = $d if $k->{dir} } }

  # files
  for my $f (@files) { $put->($f->{extent}, $f->{data}) }

  # path tables
  my ($tl, $tm) = ('', '');
  for my $d (@dirs) {
    my $id = $d->{isroot} ? chr(0) : $d->{iso};
    $tl .= pack('CC', length($id), 0) . pack('V', $d->{extent})
         . pack('v', $d->{parentno}) . $id . (length($id) % 2 ? '' : chr(0));
    $tm .= pack('CC', length($id), 0) . pack('N', $d->{extent})
         . pack('n', $d->{parentno}) . $id . (length($id) % 2 ? '' : chr(0));
  }
  $put->($ptl, $tl);
  $put->($ptm, $tm);

  # the primary volume descriptor, and a root record inside it
  my $rootrec = pack('C', 34) . pack('C', 0) . both32($root->{extent})
              . both32($root->{len}) . dirdate() . pack('C', 2)
              . pack('CC', 0, 0) . both16(1) . pack('C', 1) . chr(0);
  my $pvd = pack('C', 1) . 'CD001' . pack('C', 1) . chr(0)
          . pad('PM-DOS', 32) . pad($o{volid}, 32) . (chr(0) x 8)
          . both32($total) . (chr(0) x 32)
          . both16(1) . both16(1) . both16($BLK)
          . both32($ptlen) . pack('V', $ptl) . pack('V', 0)
          . pack('N', $ptm) . pack('N', 0)
          . $rootrec
          . pad('', 128) . pad('PM-DOS TEST DISC', 128)
          . pad('TOOLS\MKCD.PL', 128) . pad('', 128)
          . pad('', 37) . pad('', 37) . pad('', 37)
          . voldate() . voldate() . (chr(0) x 17) . (chr(0) x 17)
          . pack('C', 1) . chr(0);
  $put->($pvdblk, $pvd);
  $put->($termblk, pack('C', 255) . 'CD001' . pack('C', 1));

  # ---- flatten, from the track's own first block ----
  my $out = '';
  for my $b ($base .. $total - 1) {
    $out .= exists $img{$b} ? $img{$b} : (chr(0) x $BLK);
  }
  return { bytes=>$out, blocks=>$total - $base, total=>$total, root=>$root };
}

#=====================================================================
# Audio.  A tone, as raw 16-bit stereo at 44.1kHz, which is what a CD
# audio track is - and the .CUE sheet's BINARY keyword means exactly
# these bytes in this order.
#
# The fade at each end is not decoration: a track that starts at full
# amplitude begins with a click, and a click is indistinguishable from
# the sound of something going wrong.
#=====================================================================
sub tone {
  my ($secs, $hz) = @_;
  my $rate = 44100;
  my $n = $secs * $rate;
  my $out = '';
  my $pi = 4 * atan2(1, 1);
  for my $i (0 .. $n - 1) {
    my $env = 1.0;
    $env = $i / ($rate / 4) if $i < $rate / 4;
    $env = ($n - $i) / ($rate / 4) if $n - $i < $rate / 4;
    my $s = int(9000 * $env * sin(2 * $pi * $hz * $i / $rate));
    $out .= pack('v', $s & 0xFFFF) x 2;         # the same in both channels
  }
  $out .= chr(0) x ($RAW - length($out) % $RAW) if length($out) % $RAW;
  return $out;
}

sub msf {
  my ($f) = @_;
  return sprintf('%02d:%02d:%02d', int($f / 4500), int(($f % 4500) / 75), $f % 75);
}

sub spew {
  my ($path, $bytes) = @_;
  open my $fh, '>:raw', $path or die "$path: $!\n";
  print $fh $bytes;
  close $fh;
  printf "  %-14s %9d bytes\n", (split m{[\\/]}, $path)[-1], length($bytes);
}

#=====================================================================
# THE DISCS
#=====================================================================

#--- a deep tree, to make the writer relocate something -------------
sub deep_tree {
  my $leaf = D('nine', F('deep file.txt',
        "This directory is nine levels down, which ISO9660 cannot express.\r\n"
      . "Rock Ridge moved it to /rr_moved and left a CL behind.\r\n"));
  my $t = $leaf;
  for my $n (reverse qw(two three four five six seven eight)) {
    $t = D($n, $t);
  }
  return D('one', $t);
}

sub disc_rr {
  my ($out) = @_;
  my $long = 'A Rather Long Unix File Name, With Spaces And Punctuation'
           . ' - Which Nothing In 8.3 Can Hold, And Which Is Long Enough'
           . ' That Its NM Entry Will Not Fit Inside One Directory Record'
           . ' And Has To Be Continued In A CE Area.txt';
  my $root = D('',
    F('readme.txt',
      "Lower case, and nothing else wrong with it.  Rock Ridge keeps the\r\n"
    . "case; the 8.3 field cannot, so this file has a long name that\r\n"
    . "differs from its alias only in case - and gets no ~1.\r\n"),
    F('Makefile',
      "No extension at all, and mixed case.\r\n"),
    F($long, "The name of this file is " . length($long) . " characters long.\r\n"),
    F('what? and *.txt',
      "Characters a DOS long name may not hold: they become underscores.\r\n"),
    F("caf\xc3\xa9 menu.txt",
      "The name of this file is UTF-8 - one e-acute, two bytes.\r\n"),
    D('src with spaces',
      F('main.c', "int main(void) { return 0; }\r\n"),
      F('README', "A directory whose own name needs Rock Ridge too.\r\n")),
    FS('split across extents.txt', splitdata(), 3),
    F("na\xc3\xafve r\xc3\xa9sum\xc3\xa9 \xc2\xbd.txt",
      "Three characters codepage 437 can spell - i-diaeresis, e-acute\r\n"
    . "and one half - and they are UTF-8 on the disc, two bytes each.\r\n"),
    F("\xce\xb1\xce\xb2\xce\xb3 and \xe2\x88\x9e.txt",
      "Greek letters and an infinity sign: 437 has all four of these\r\n"
    . "and a reader that folds everything above 7Eh to '_' has none.\r\n"),
    F("stra\xc3\x9fe \xe2\x82\xacuro.txt",
      "The sharp s IS in 437 and the euro sign is NOT, so this name\r\n"
    . "comes back part spelled and part underscore - which is the\r\n"
    . "honest answer and not a failure.\r\n"),
    deep_tree());
  my $img = build(root=>$root, volid=>'RRTEST', rr=>1);
  spew("$out/RRTEST.ISO", $img->{bytes});
  return $img;
}

sub disc_multi {
  my ($out) = @_;

  # ---- session one ----
  my $s1root = D('',
    F('BOTH.TXT',
      "I am written in SESSION ONE and listed by BOTH sessions.\r\n"
    . "Session two's directory points BACKWARDS at this extent, so\r\n"
    . "reading this text proves extents are absolute from the start\r\n"
    . "of the disc rather than from the start of the session.\r\n"),
    F('GONE.TXT',
      "I exist only in SESSION ONE.  If you can see me, the kernel is\r\n"
    . "reading the first session and the whole point has been missed.\r\n"),
    D('OLDDIR', F('OLD.TXT', "A session one subdirectory.\r\n")));
  my $s1 = build(root=>$s1root, volid=>'SESSION ONE');
  spew("$out/S1.BIN", $s1->{bytes});

  # ---- session two, at a real recorder's distance ----
  my $carried;
  for my $k (@{$s1->{root}{kids}}) { $carried = $k if $k->{name} eq 'BOTH.TXT' }
  my $base = $s1->{blocks} + $GAP;
  my $s2root = D('',
    { %$carried, carried=>1 },                  # same extent, same size
    F('NEW.TXT',
      "I am written in SESSION TWO.  Seeing me AND BOTH.TXT, and NOT\r\n"
    . "seeing GONE.TXT, is the whole test: the newest session is the\r\n"
    . "one that counts, and it can still reach the older one's data.\r\n"),
    D('NEWDIR', F('NEW2.TXT', "A session two subdirectory.\r\n")));
  my $s2 = build(root=>$s2root, volid=>'SESSION TWO', base=>$base);
  spew("$out/S2.BIN", $s2->{bytes});

  open my $cue, '>', "$out/MSESS.CUE" or die $!;
  print $cue "FILE \"S1.BIN\" BINARY\r\n";
  print $cue "  TRACK 01 MODE1/2048\r\n    INDEX 01 00:00:00\r\n";
  print $cue "FILE \"S2.BIN\" BINARY\r\n";
  print $cue "  TRACK 02 MODE1/2048\r\n";
  print $cue "    PREGAP ", msf($GAP), "\r\n    INDEX 01 00:00:00\r\n";
  close $cue;
  print "  MSESS.CUE      session two starts at LBA $base\n";
}

sub disc_extra {
  my ($out) = @_;
  # Audio first, data second - a CD Extra, and the disc that does not
  # mount at all today: there is no CD001 at block 16 because block 16
  # is music.
  my $a1 = tone(20, 440);
  my $a2 = tone(15, 660);
  spew("$out/XA1.BIN", $a1);
  spew("$out/XA2.BIN", $a2);
  my $audio = length($a1) / $RAW + 150 + length($a2) / $RAW;
  my $base = $audio + $GAP;
  my $root = D('',
    F('README.TXT',
      "The data session of a CD Extra.  Two audio tracks come first,\r\n"
    . "so block 16 of this disc is music and the descriptors are at\r\n"
    . "block " . ($base + 16) . " instead.\r\n"),
    D('DATA', F('DATA.TXT', "Data, on a disc that begins with audio.\r\n")));
  my $img = build(root=>$root, volid=>'CD EXTRA', base=>$base);
  spew("$out/XDATA.BIN", $img->{bytes});

  open my $cue, '>', "$out/CDEXTRA.CUE" or die $!;
  print $cue "FILE \"XA1.BIN\" BINARY\r\n";
  print $cue "  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n";
  print $cue "FILE \"XA2.BIN\" BINARY\r\n";
  print $cue "  TRACK 02 AUDIO\r\n    PREGAP 00:02:00\r\n    INDEX 01 00:00:00\r\n";
  print $cue "FILE \"XDATA.BIN\" BINARY\r\n";
  print $cue "  TRACK 03 MODE1/2048\r\n    PREGAP ", msf($GAP),
             "\r\n    INDEX 01 00:00:00\r\n";
  close $cue;
  print "  CDEXTRA.CUE    data track starts at LBA $base\n";
}

sub disc_mixed {
  my ($out) = @_;
  # Data first, audio after: a game with CD music, which is the layout
  # the audio half of this release exists for.
  my $root = D('',
    F('README.TXT',
      "Track one of a mixed-mode disc.  Tracks two and three are\r\n"
    . "music, and CDATEST plays them through the MSCDEX API.\r\n"),
    F('PLAY.TXT', "Track 2 is 440Hz for 20s; track 3 is 660Hz for 15s.\r\n"),
    FS('SPLIT.TXT', splitdata(), 3));
  my $img = build(root=>$root, volid=>'MIXED MODE');
  spew("$out/MDATA.BIN", $img->{bytes});
  my $a1 = tone(20, 440);
  my $a2 = tone(15, 660);
  spew("$out/MA1.BIN", $a1);
  spew("$out/MA2.BIN", $a2);

  open my $cue, '>', "$out/MIXED.CUE" or die $!;
  print $cue "FILE \"MDATA.BIN\" BINARY\r\n";
  print $cue "  TRACK 01 MODE1/2048\r\n    INDEX 01 00:00:00\r\n";
  print $cue "FILE \"MA1.BIN\" BINARY\r\n";
  print $cue "  TRACK 02 AUDIO\r\n    PREGAP 00:02:00\r\n    INDEX 01 00:00:00\r\n";
  print $cue "FILE \"MA2.BIN\" BINARY\r\n";
  print $cue "  TRACK 03 AUDIO\r\n    INDEX 01 00:00:00\r\n";
  close $cue;
  print "  MIXED.CUE      data track 1, audio tracks 2 and 3\n";
}

sub disc_rrdeep {
  my ($out) = @_;
  # THE SAME RELOCATION, SHALLOW ENOUGH TO REACH.  A directory is
  # relocated because ISO9660 allows only eight levels - but PM-DOS's
  # own path walker stops at about that many components too, so a disc
  # that relocates exactly where the standard says to puts the result
  # somewhere the kernel cannot walk to in order to look at it.  This
  # disc relocates at the fourth level instead: structurally identical
  # - the same CL, RE and PL entries in the same places - and reachable
  # by a path short enough to type.
  local $RELOC = 4;
  my $root = D('',
    F('readme.txt', "The shallow relocation disc.\r\n"),
    D('one', D('two', D('three',
        F('inner.txt', "Three levels down, and not relocated.\r\n"),
        D('four', F('moved file.txt',
            "This directory was RELOCATED to /rr_moved and is reached\r\n"
          . "through a CL entry left behind in its place.  CD .. out of\r\n"
          . "it should go back to three rather than to the holding pen,\r\n"
          . "which is what the PL entry is for.\r\n"))))));
  my $img = build(root=>$root, volid=>'RRDEEP', rr=>1);
  spew("$out/RRDEEP.ISO", $img->{bytes});
}

#=====================================================================
my $what = shift || 'all';
my $out  = shift || 'S:/Software/PMDOSCD';
mkdir $out unless -d $out;
$out =~ s{\\}{/}g;
print "mkcd: $what -> $out\n";
disc_rr($out)     if $what eq 'rr'     or $what eq 'all';
disc_rrdeep($out) if $what eq 'rrdeep' or $what eq 'all';
disc_multi($out) if $what eq 'multi' or $what eq 'all';
disc_extra($out) if $what eq 'extra' or $what eq 'all';
disc_mixed($out) if $what eq 'mixed' or $what eq 'all';
print "done.\n";
