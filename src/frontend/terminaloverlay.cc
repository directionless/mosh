/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/typeof/typeof.hpp>
#include <algorithm>
#include <wchar.h>
#include <list>
#include <typeinfo>
#include <limits.h>

#include "terminaloverlay.h"

using namespace boost::lambda;
using namespace Overlay;
using std::max;

void ConditionalOverlayCell::apply( Framebuffer &fb, uint64_t confirmed_epoch, int row, bool flag ) const
{
  if ( (!active)
       || (row >= fb.ds.get_height())
       || (col >= fb.ds.get_width()) ) {
    return;
  }

  if ( tentative( confirmed_epoch ) ) {
    return;
  }

  if ( unknown ) {
    if ( flag && ( col != fb.ds.get_width() - 1 ) ) {
      fb.get_mutable_cell( row, col )->renditions.underlined = true;
    }
    return;
  }

  if ( !(*(fb.get_cell( row, col )) == replacement) ) {
    if ( replacement.is_blank() && fb.get_cell( row, col )->is_blank() ) {
      return;
    }

    *(fb.get_mutable_cell( row, col )) = replacement;
    if ( flag ) {
      fb.get_mutable_cell( row, col )->renditions.underlined = true;
    }
  }
}

Validity ConditionalOverlayCell::get_validity( const Framebuffer &fb, int row,
					       uint64_t early_ack __attribute__((unused)),
					       uint64_t late_ack ) const
{
  if ( !active ) {
    return Inactive;
  }

  if ( (row >= fb.ds.get_height())
       || (col >= fb.ds.get_width()) ) {
    return IncorrectOrExpired;
  }

  const Cell &current = *( fb.get_cell( row, col ) );

  /* see if it hasn't been updated yet */
  if ( late_ack >= expiration_frame ) {
    if ( unknown ) {
      return CorrectNoCredit;
    }

    if ( replacement.is_blank() ) { /* too easy for this to trigger falsely */
      return CorrectNoCredit;
    }

    if ( (current.contents == replacement.contents)
	 || (current.is_blank() && replacement.is_blank()) ) {
      BOOST_AUTO( it, find_if( original_contents.begin(), original_contents.end(),
			       (replacement.is_blank() && bind( &Cell::is_blank, _1 ))
			       || replacement.contents == (&_1)->*&Cell::contents ) );
      if ( it == original_contents.end() ) {
	return Correct;
      } else {
	return CorrectNoCredit;
      }
    } else {
      return IncorrectOrExpired;
    }
  }

  return Pending;
}

Validity ConditionalCursorMove::get_validity( const Framebuffer &fb,
					      uint64_t early_ack __attribute((unused)),
					      uint64_t late_ack ) const
{
  if ( !active ) {
    return Inactive;
  }

  if ( (row >= fb.ds.get_height())
       || (col >= fb.ds.get_width()) ) {
    assert( false );
    //    fprintf( stderr, "Crazy cursor (%d,%d)!\n", row, col );
    return IncorrectOrExpired;
  }

  if ( late_ack >= expiration_frame ) {
    if ( (fb.ds.get_cursor_col() == col)
	 && (fb.ds.get_cursor_row() == row) ) {
      return Correct;
    } else {
      return IncorrectOrExpired;
    }
  }

  return Pending;
}

void ConditionalCursorMove::apply( Framebuffer &fb, uint64_t confirmed_epoch ) const
{
  if ( !active ) {
    return;
  }

  if ( tentative( confirmed_epoch ) ) {
    return;
  }

  assert( row < fb.ds.get_height() );
  assert( col < fb.ds.get_width() );
  assert( !fb.ds.origin_mode );

  fb.ds.move_row( row, false );
  fb.ds.move_col( col, false, false );
}

NotificationEngine::NotificationEngine()
  : last_word_from_server( timestamp() ),
    message(),
    message_expiration( -1 )
{}

void NotificationEngine::apply( Framebuffer &fb ) const
{
  uint64_t now = timestamp();

  bool time_expired = need_countup( now );

  if ( message.empty() && !time_expired ) {
    return;
  }

  assert( fb.ds.get_width() > 0 );
  assert( fb.ds.get_height() > 0 );

  /* hide cursor if necessary */
  if ( fb.ds.get_cursor_row() == 0 ) {
    fb.ds.cursor_visible = false;
  }

  /* draw bar across top of screen */
  Cell notification_bar( 0 );
  notification_bar.renditions.foreground_color = 37;
  notification_bar.renditions.background_color = 44;
  notification_bar.contents.push_back( 0x20 );

  for ( int i = 0; i < fb.ds.get_width(); i++ ) {
    *(fb.get_mutable_cell( 0, i )) = notification_bar;
  }

  /* write message */
  wchar_t tmp[ 128 ];

  if ( message.empty() && (!time_expired) ) {
    return;
  } else if ( message.empty() && time_expired ) {
    swprintf( tmp, 128, L"mosh: Last contact %.0f seconds ago. [To quit: Ctrl-^ .]", (double)(now - last_word_from_server) / 1000.0 );
  } else if ( (!message.empty()) && (!time_expired) ) {
    swprintf( tmp, 128, L"mosh: %ls [To quit: Ctrl-^ .]", message.c_str() );
  } else {
    swprintf( tmp, 128, L"mosh: %ls (%.0f s without contact.) [To quit: Ctrl-^ .]", message.c_str(),
	      (double)(now - last_word_from_server) / 1000.0 );
  }

  wstring string_to_draw( tmp );

  int overlay_col = 0;

  Cell *combining_cell = fb.get_mutable_cell( 0, 0 );

  /* We unfortunately duplicate the terminal's logic for how to render a Unicode sequence into graphemes */
  for ( wstring::const_iterator i = string_to_draw.begin(); i != string_to_draw.end(); i++ ) {
    if ( overlay_col >= fb.ds.get_width() ) {
      break;
    }

    wchar_t ch = *i;
    int chwidth = ch == L'\0' ? -1 : wcwidth( ch );
    Cell *this_cell = 0;

    switch ( chwidth ) {
    case 1: /* normal character */
    case 2: /* wide character */
      this_cell = fb.get_mutable_cell( 0, overlay_col );
      fb.reset_cell( this_cell );
      this_cell->renditions.bold = true;
      this_cell->renditions.foreground_color = 37;
      this_cell->renditions.background_color = 44;
      
      this_cell->contents.push_back( ch );
      this_cell->width = chwidth;
      combining_cell = this_cell;

      overlay_col += chwidth;
      break;
    case 0: /* combining character */
      if ( !combining_cell ) {
	break;
      }

      if ( combining_cell->contents.size() == 0 ) {
	assert( combining_cell->width == 1 );
	combining_cell->fallback = true;
	overlay_col++;
      }

      if ( combining_cell->contents.size() < 16 ) {
	combining_cell->contents.push_back( ch );
      }
      break;
    case -1: /* unprintable character */
      break;
    default:
      assert( false );
    }
  }
}

void NotificationEngine::adjust_message( void )
{
  if ( timestamp() >= message_expiration ) {
    message.clear();
  }  
}

void OverlayManager::apply( Framebuffer &fb )
{
  predictions.cull( fb );
  predictions.apply( fb );
  notifications.adjust_message();
  notifications.apply( fb );
  title.apply( fb );
}

int OverlayManager::wait_time( void )
{
  uint64_t next_expiry = INT_MAX;

  uint64_t now = timestamp();

  uint64_t message_delay = notifications.get_message_expiration() - now;

  if ( message_delay < next_expiry ) {
    next_expiry = message_delay;
  }

  if ( notifications.need_countup( now ) && ( next_expiry > 1000 ) ) {
    next_expiry = 1000;
  }

  if ( predictions.active() && ( next_expiry > 20 ) ) {
    next_expiry = 20;
  }

  return next_expiry;
}

void TitleEngine::set_prefix( const wstring s )
{
  prefix = deque<wchar_t>( s.begin(), s.end() );
}

void ConditionalOverlayRow::apply( Framebuffer &fb, uint64_t confirmed_epoch, bool flag ) const
{
  for_each( overlay_cells.begin(), overlay_cells.end(), bind( &ConditionalOverlayCell::apply, _1, var(fb), confirmed_epoch, row_num, flag ) );
}

void PredictionEngine::apply( Framebuffer &fb ) const
{
  bool show = (display_preference != Never) && ( srtt_trigger
						 || glitch_trigger
						 || (display_preference == Always) );

  if ( show ) {
    for_each( cursors.begin(), cursors.end(), bind( &ConditionalCursorMove::apply, _1, var(fb), confirmed_epoch ) );

    for_each( overlays.begin(), overlays.end(), bind( &ConditionalOverlayRow::apply, _1, var(fb), confirmed_epoch, flagging ) );
  }
}

void PredictionEngine::kill_epoch( uint64_t epoch, const Framebuffer &fb )
{
  cursors.remove_if( bind( &ConditionalCursorMove::tentative, _1, epoch - 1 ) );

  cursors.push_back( ConditionalCursorMove( local_frame_sent + 1,
					    fb.ds.get_cursor_row(),
					    fb.ds.get_cursor_col(),
					    prediction_epoch ) );
  cursor().active = true;

  for ( BOOST_AUTO( i, overlays.begin() ); i != overlays.end(); i++ ) {
    for ( BOOST_AUTO( j, i->overlay_cells.begin() ); j != i->overlay_cells.end(); j++ ) {
      if ( j->tentative( epoch - 1 ) ) {
	j->reset();
      }
    }
  }

  become_tentative();
}

void PredictionEngine::reset( void )
{
  cursors.clear();
  overlays.clear();
  become_tentative();

  //  fprintf( stderr, "RESETTING\n" );
}

void PredictionEngine::init_cursor( const Framebuffer &fb )
{
  if ( cursors.empty() ) {
    /* initialize new cursor prediction */
    
    cursors.push_back( ConditionalCursorMove( local_frame_sent + 1,
					      fb.ds.get_cursor_row(),
					      fb.ds.get_cursor_col(),
					      prediction_epoch ) );

    cursor().active = true;
  } else if ( cursor().tentative_until_epoch != prediction_epoch ) {
    cursors.push_back( ConditionalCursorMove( local_frame_sent + 1,
					      cursor().row,
					      cursor().col,
					      prediction_epoch ) );

    cursor().active = true;
  }
}

void PredictionEngine::cull( const Framebuffer &fb )
{
  if ( display_preference == Never ) {
    return;
  }

  uint64_t now = timestamp();

  /* control srtt_trigger with hysteresis */
  if ( send_interval > SRTT_TRIGGER_HIGH ) {
    srtt_trigger = true;
  } else if ( send_interval <= SRTT_TRIGGER_LOW ) { /* 20 ms is current minimum value */
    srtt_trigger = false;
  }

  /* control flagging with hysteresis */
  if ( send_interval > FLAG_TRIGGER_HIGH ) {
    flagging = true;
  } else if ( send_interval <= FLAG_TRIGGER_LOW ) {
    flagging = false;
  }

  /* go through cell predictions */

  BOOST_AUTO( i, overlays.begin() );
  while ( i != overlays.end() ) {
    BOOST_AUTO( inext, i );
    inext++;
    if ( (i->row_num < 0) || (i->row_num >= fb.ds.get_height()) ) {
      overlays.erase( i );
      i = inext;
      continue;
    }

    for ( BOOST_AUTO( j, i->overlay_cells.begin() ); j != i->overlay_cells.end(); j++ ) {
      switch ( j->get_validity( fb, i->row_num,
				local_frame_acked, local_frame_late_acked ) ) {
      case IncorrectOrExpired:
	if ( j->tentative( confirmed_epoch ) ) {

	  /*
	  fprintf( stderr, "Bad tentative prediction in row %d, col %d (think %lc, actually %lc)\n",
		   i->row_num, j->col,
		   j->replacement.debug_contents(),
		   fb.get_cell( i->row_num, j->col )->debug_contents()
		   );
	  */

	  kill_epoch( j->tentative_until_epoch, fb );
	  /*
	  if ( j->display_time != uint64_t(-1) ) {
	    fprintf( stderr, "TIMING %ld - %ld (TENT)\n", time(NULL), now - j->display_time );
	  }
	  */
	} else {
	  /*
	  fprintf( stderr, "[%d=>%d] Killing prediction in row %d, col %d (think %lc, actually %lc)\n",
		   (int)local_frame_acked, (int)j->expiration_frame,
		   i->row_num, j->col,
		   j->replacement.debug_contents(),
		   fb.get_cell( i->row_num, j->col )->debug_contents() );
	  */
	  /*
	  if ( j->display_time != uint64_t(-1) ) {
	    fprintf( stderr, "TIMING %ld - %ld\n", time(NULL), now - j->display_time );
	  }
	  */

	  reset();
	  return;
	}
	break;
      case Correct:
	/*
	if ( j->display_time != uint64_t(-1) ) {
	  fprintf( stderr, "TIMING %ld + %ld\n", now, now - j->display_time );
	}
	*/

	if ( j->tentative_until_epoch > confirmed_epoch ) {
	  confirmed_epoch = j->tentative_until_epoch;

	  /*
	  fprintf( stderr, "%lc in (%d,%d) confirms epoch %lu (predicting in epoch %lu)\n",
		   j->replacement.debug_contents(), i->row_num, j->col,
		   confirmed_epoch, prediction_epoch );
	  */

	}

	/* When predictions come in quickly, slowly take away the glitch trigger. */
	if ( (now - j->prediction_time) < GLITCH_THRESHOLD ) {
	  if ( (glitch_trigger > 0) && (now - GLITCH_REPAIR_MININTERVAL >= last_quick_confirmation) ) {
	    glitch_trigger--;
	    last_quick_confirmation = now;
	  }
	}

	/* no break */
      case CorrectNoCredit:
	j->reset();

	break;
      case Pending:
	/* When a prediction takes a long time to be confirmed, we
	   activate the predictions even if SRTT is low */
	if ( (now - j->prediction_time) >= GLITCH_THRESHOLD ) {
	  glitch_trigger = GLITCH_REPAIR_COUNT;
	}

	break;
      default:
	break;
      }
    }

    i = inext;
  }

  /* go through cursor predictions */
  if ( !cursors.empty() ) {
    if ( cursor().get_validity( fb,
				local_frame_acked, local_frame_late_acked ) == IncorrectOrExpired ) {
      /*
      fprintf( stderr, "Sadly, we're predicting (%d,%d) vs. (%d,%d) [tau: %ld, expiration_time=%ld, now=%ld]\n",
	       cursor().row, cursor().col,
	       fb.ds.get_cursor_row(),
	       fb.ds.get_cursor_col(),
	       cursor().tentative_until_epoch,
	       cursor().expiration_time,
	       now );
      */
      reset();
      return;
    }
  }

  cursors.remove_if( bind( &ConditionalCursorMove::get_validity, _1, var(fb),
			   local_frame_acked, local_frame_late_acked ) != Pending );
}

ConditionalOverlayRow & PredictionEngine::get_or_make_row( int row_num, int num_cols )
{
  BOOST_AUTO( it, find_if( overlays.begin(), overlays.end(),
			   (&_1)->*&ConditionalOverlayRow::row_num == row_num ) );

  if ( it != overlays.end() ) {
    return *it;
  } else {
    /* make row */
    ConditionalOverlayRow r( row_num );
    r.overlay_cells.reserve( num_cols );
    for ( int i = 0; i < num_cols; i++ ) {
      r.overlay_cells.push_back( ConditionalOverlayCell( 0, i, prediction_epoch ) );
      assert( r.overlay_cells[ i ].col == i );
    }
    overlays.push_back( r );
    return overlays.back();
  }
}

void PredictionEngine::new_user_byte( char the_byte, const Framebuffer &fb )
{
  if ( display_preference == Never ) {
    return;
  }

  cull( fb );

  uint64_t now = timestamp();

  /* translate application-mode cursor control function to ANSI cursor control sequence */
  if ( (last_byte == 0x1b)
       && (the_byte == 'O') ) {
    the_byte = '[';
  }
  last_byte = the_byte;

  list<Parser::Action *> actions( parser.input( the_byte ) );

  for ( BOOST_AUTO( it, actions.begin() ); it != actions.end(); it++ ) {
    Parser::Action *act = *it;

    /*
    fprintf( stderr, "Action: %s (%lc)\n",
	     act->name().c_str(), act->char_present ? act->ch : L'_' );
    */

    if ( typeid( *act ) == typeid( Parser::Print ) ) {
      /* make new prediction */

      init_cursor( fb );

      assert( act->char_present );

      wchar_t ch = act->ch;
      /* XXX handle wide characters */

      if ( ch == 0x7f ) { /* backspace */
	//	fprintf( stderr, "Backspace.\n" );
	ConditionalOverlayRow &the_row = get_or_make_row( cursor().row, fb.ds.get_width() );

	if ( cursor().col > 0 ) {
	  cursor().col--;
	  cursor().expire( local_frame_sent + 1, now );

	  for ( int i = cursor().col; i < fb.ds.get_width(); i++ ) {
	    ConditionalOverlayCell &cell = the_row.overlay_cells[ i ];
	    
	    cell.reset_with_orig();
	    cell.active = true;
	    cell.tentative_until_epoch = prediction_epoch;
	    cell.expire( local_frame_sent + 1, now );
	    cell.original_contents.push_back( *fb.get_cell( cursor().row, i ) );
	  
	    if ( i + 2 < fb.ds.get_width() ) {
	      ConditionalOverlayCell &next_cell = the_row.overlay_cells[ i + 1 ];
	      const Cell *next_cell_actual = fb.get_cell( cursor().row, i + 1 );

	      if ( next_cell.active ) {
		if ( next_cell.unknown ) {
		  cell.unknown = true;
		} else {
		  cell.unknown = false;
		  cell.replacement = next_cell.replacement;
		}
	      } else {
		cell.unknown = false;
		cell.replacement = *next_cell_actual;
	      }
	    } else {
	      cell.unknown = true;
	    }
	  }
	}
      } else if ( (ch < 0x20) || (wcwidth( ch ) != 1) ) {
	/* unknown print */
	become_tentative();
	//	fprintf( stderr, "Unknown print 0x%x\n", ch );
      } else {
	assert( cursor().row >= 0 );
	assert( cursor().col >= 0 );
	assert( cursor().row < fb.ds.get_height() );
	assert( cursor().col < fb.ds.get_width() );

	ConditionalOverlayRow &the_row = get_or_make_row( cursor().row, fb.ds.get_width() );

	if ( cursor().col + 1 >= fb.ds.get_width() ) {
	  /* prediction in the last column is tricky */
	  /* e.g., emacs will show wrap character, shell will just put the character there */
	  become_tentative();
	}

	/* do the insert */
	for ( int i = fb.ds.get_width() - 1; i > cursor().col; i-- ) {
	  ConditionalOverlayCell &cell = the_row.overlay_cells[ i ];
	  cell.reset_with_orig();
	  cell.active = true;
	  cell.tentative_until_epoch = prediction_epoch;
	  cell.expire( local_frame_sent + 1, now );
	  cell.original_contents.push_back( *fb.get_cell( cursor().row, i ) );

	  ConditionalOverlayCell &prev_cell = the_row.overlay_cells[ i - 1 ];
	  const Cell *prev_cell_actual = fb.get_cell( cursor().row, i - 1 );

	  if ( i == fb.ds.get_width() - 1 ) {
	    cell.unknown = true;
	  } else if ( prev_cell.active ) {
	    if ( prev_cell.unknown ) {
	      cell.unknown = true;
	    } else {
	      cell.unknown = false;
	      cell.replacement = prev_cell.replacement;
	    }
	  } else {
	    cell.unknown = false;
	    cell.replacement = *prev_cell_actual;
	  }
	}
	
	ConditionalOverlayCell &cell = the_row.overlay_cells[ cursor().col ];
	cell.reset_with_orig();
	cell.active = true;
	cell.tentative_until_epoch = prediction_epoch;
	cell.expire( local_frame_sent + 1, now );
	cell.replacement.renditions = fb.ds.get_renditions();
	cell.replacement.contents.clear();
	cell.replacement.contents.push_back( ch );
	cell.original_contents.push_back( *fb.get_cell( cursor().row, cursor().col ) );

	/*
	fprintf( stderr, "[%d=>%d] Predicting %lc in row %d, col %d [tue: %lu]\n",
		 (int)local_frame_acked, (int)cell.expiration_frame,
		 ch, cursor().row, cursor().col,
		 cell.tentative_until_epoch );
	*/

	cursor().expire( local_frame_sent + 1, now );

	/* do we need to wrap? */
	if ( cursor().col < fb.ds.get_width() - 1 ) {
	  cursor().col++;
	} else {
	  become_tentative();
	  newline_carriage_return( fb );
	}
      }
    } else if ( typeid( *act ) == typeid( Parser::Execute ) ) {
      if ( act->char_present && (act->ch == 0x0d) /* CR */ ) {
	become_tentative();
	newline_carriage_return( fb );
      } else {
	//	fprintf( stderr, "Execute 0x%x\n", act->ch );
	become_tentative();	
      }
    } else if ( typeid( *act ) == typeid( Parser::Esc_Dispatch ) ) {
      //      fprintf( stderr, "Escape sequence\n" );
      become_tentative();
    } else if ( typeid( *act ) == typeid( Parser::CSI_Dispatch ) ) {
      if ( act->char_present && (act->ch == L'C') ) { /* right arrow */
	init_cursor( fb );
	if ( cursor().col < fb.ds.get_width() - 1 ) {
	  cursor().col++;
	  cursor().expire( local_frame_sent + 1, now );
	}
      } else if ( act->char_present && (act->ch == L'D') ) { /* left arrow */
	init_cursor( fb );
	
	if ( cursor().col > 0 ) {
	  cursor().col--;
	  cursor().expire( local_frame_sent + 1, now );
	}
      } else {
	//	fprintf( stderr, "CSI sequence %lc\n", act->ch );
	become_tentative();
      }
    } else if ( typeid( *act ) == typeid( Parser::Clear ) ) {

    }

    delete act;
  }
}

void PredictionEngine::newline_carriage_return( const Framebuffer &fb )
{
  uint64_t now = timestamp();
  init_cursor( fb );
  cursor().col = 0;
  if ( cursor().row == fb.ds.get_height() - 1 ) {
    for ( BOOST_AUTO( i, overlays.begin() ); i != overlays.end(); i++ ) {
      i->row_num--;
      for ( BOOST_AUTO( j, i->overlay_cells.begin() ); j != i->overlay_cells.end(); j++ ) {
	if ( j->active ) {
	  j->expire( local_frame_sent + 1, now );
	}
      }
    }

    /* make blank prediction for last row */
    ConditionalOverlayRow &the_row = get_or_make_row( cursor().row, fb.ds.get_width() );
    for ( BOOST_AUTO( j, the_row.overlay_cells.begin() ); j != the_row.overlay_cells.end(); j++ ) {
      j->active = true;
      j->tentative_until_epoch = prediction_epoch;
      j->expire( local_frame_sent + 1, now );
      j->replacement.contents.clear();
    }
  } else {
    cursor().row++;
  }
}

void PredictionEngine::become_tentative( void )
{
  prediction_epoch++;

  /*
  fprintf( stderr, "Now tentative in epoch %lu (confirmed=%lu)\n",
	   prediction_epoch, confirmed_epoch );
  */
}

bool PredictionEngine::active( void ) const
{
  if ( !cursors.empty() ) {
    return true;
  }

  for ( BOOST_AUTO( i, overlays.begin() ); i != overlays.end(); i++ ) {  
    for ( BOOST_AUTO( j, i->overlay_cells.begin() ); j != i->overlay_cells.end(); j++ ) {  
      if ( j->active ) {
	return true;
      }
    }
  }

  return false;
}
