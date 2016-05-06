/*****************************************************************************
 * virtual_segment.cpp : virtual segment implementation in the MKV demuxer
 *****************************************************************************
 * Copyright © 2003-2011 VideoLAN and VLC authors
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Steve Lhomme <steve.lhomme@free.fr>
 *          Denis Charmet <typx@dinauz.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#include <vector>
#include <new>

#include "demux.hpp"

/* FIXME move this */
matroska_segment_c * getSegmentbyUID( KaxSegmentUID * p_uid, std::vector<matroska_segment_c*> & segments )
{
    for( size_t i = 0; i < segments.size(); i++ )
    {
        if( segments[i]->p_segment_uid &&
            *p_uid == *(segments[i]->p_segment_uid) )
            return segments[i];
    }
    return NULL;
}

virtual_chapter_c * virtual_chapter_c::CreateVirtualChapter( chapter_item_c * p_chap,
                                                             matroska_segment_c & main_segment,
                                                             std::vector<matroska_segment_c*> & segments,
                                                             int64_t & usertime_offset, bool b_ordered)
{
    std::vector<virtual_chapter_c *> sub_chapters;
    if( !p_chap )
    {
        /* Dummy chapter use the whole segment */
        return new (std::nothrow) virtual_chapter_c( main_segment, NULL, 0, main_segment.i_duration * 1000, sub_chapters );
    }

    matroska_segment_c * p_segment = &main_segment;
    if( p_chap->p_segment_uid &&
       ( !( p_segment = getSegmentbyUID( (KaxSegmentUID*) p_chap->p_segment_uid,segments ) ) || !b_ordered ) )
    {
        msg_Warn( &main_segment.sys.demuxer,
                  "Couldn't find segment 0x%x or not ordered... - ignoring chapter %s",
                  *( (uint32_t *) p_chap->p_segment_uid->GetBuffer() ),p_chap->psz_name.c_str() );
        return NULL;
    }

    /* Preload segment */
    if ( !p_segment->b_preloaded )
        p_segment->Preload();

    int64_t start = ( b_ordered )? usertime_offset : p_chap->i_start_time;
    int64_t tmp = usertime_offset;

    for( size_t i = 0; i < p_chap->sub_chapters.size(); i++ )
    {
        virtual_chapter_c * p_vsubchap = CreateVirtualChapter( p_chap->sub_chapters[i], *p_segment, segments, tmp, b_ordered );

        if( p_vsubchap )
            sub_chapters.push_back( p_vsubchap );
    }
    int64_t stop = ( b_ordered )?
            (((p_chap->i_end_time == -1 ||
               (p_chap->i_end_time - p_chap->i_start_time) < (tmp - usertime_offset) )) ? tmp :
             p_chap->i_end_time - p_chap->i_start_time + usertime_offset )
            :p_chap->i_end_time;

    virtual_chapter_c * p_vchap = new (std::nothrow) virtual_chapter_c( *p_segment, p_chap, start, stop, sub_chapters );
    if( !p_vchap )
    {
        for( size_t i = 0 ; i < sub_chapters.size(); i++ )
            delete sub_chapters[i];
        return NULL;
    }

    if ( p_chap->i_end_time >= 0 )
        usertime_offset += p_chap->i_end_time - p_chap->i_start_time;
    else
        usertime_offset = tmp;

    msg_Dbg( &main_segment.sys.demuxer,
             "Virtual chapter %s from %" PRId64 " to %" PRId64 " - " ,
             p_chap->psz_name.c_str(), p_vchap->i_mk_virtual_start_time, p_vchap->i_mk_virtual_stop_time );

    return p_vchap;
}

virtual_chapter_c::~virtual_chapter_c()
{
    for( size_t i = 0 ; i < sub_vchapters.size(); i++ )
        delete sub_vchapters[i];
}


virtual_edition_c::virtual_edition_c( chapter_edition_c * p_edit, matroska_segment_c & main_segment, std::vector<matroska_segment_c*> & opened_segments)
{
    bool b_fake_ordered = false;
    p_edition = p_edit;
    b_ordered = false;

    int64_t usertime_offset = 0;

    /* ordered chapters */
    if( p_edition && p_edition->b_ordered )
    {
        b_ordered = true;
        for( size_t i = 0; i < p_edition->sub_chapters.size(); i++ )
        {
            virtual_chapter_c * p_vchap = virtual_chapter_c::CreateVirtualChapter( p_edition->sub_chapters[i],
                                                                                   main_segment, opened_segments,
                                                                                   usertime_offset, b_ordered );
            if( p_vchap )
                vchapters.push_back( p_vchap );
        }
        if( vchapters.size() )
            i_duration = vchapters[ vchapters.size() - 1 ]->i_mk_virtual_stop_time;
        else
            i_duration = 0; /* Empty ordered editions will be ignored */
    }
    else /* Not ordered or no edition at all */
    {
        matroska_segment_c * p_cur = &main_segment;
        virtual_chapter_c * p_vchap = NULL;
        int64_t tmp = 0;

        /* check for prev linked segments */
        /* FIXME to avoid infinite recursion we limit to 10 prev should be better as parameter */
        for( int limit = 0; limit < 10 && p_cur->p_prev_segment_uid ; limit++ )
        {
            matroska_segment_c * p_prev = NULL;
            if( ( p_prev = getSegmentbyUID( p_cur->p_prev_segment_uid, opened_segments ) ) )
            {
                tmp = 0;
                msg_Dbg( &main_segment.sys.demuxer, "Prev segment 0x%x found\n",
                         *(int32_t*)p_cur->p_prev_segment_uid->GetBuffer() );

                /* Preload segment */
                if ( !p_prev->b_preloaded )
                    p_prev->Preload();

                /* Create virtual_chapter from the first edition if any */
                chapter_item_c * p_chap = ( p_prev->stored_editions.size() > 0 )? ((chapter_item_c *)p_prev->stored_editions[0]) : NULL;

                p_vchap = virtual_chapter_c::CreateVirtualChapter( p_chap, *p_prev, opened_segments, tmp, b_ordered );

                if( p_vchap )
                    vchapters.insert( vchapters.begin(), p_vchap );

                p_cur = p_prev;
                b_fake_ordered = true;
            }
            else /* segment not found */
                break;
        }

        tmp = 0;

        /* Append the main segment */
        p_vchap = virtual_chapter_c::CreateVirtualChapter( (chapter_item_c*) p_edit, main_segment,
                                                           opened_segments, tmp, b_ordered );
        if( p_vchap )
            vchapters.push_back( p_vchap );

        /* Append next linked segments */
        for( int limit = 0; limit < 10 && p_cur->p_next_segment_uid; limit++ )
        {
            matroska_segment_c * p_next = NULL;
            if( ( p_next = getSegmentbyUID( p_cur->p_next_segment_uid, opened_segments ) ) )
            {
                tmp = 0;
                msg_Dbg( &main_segment.sys.demuxer, "Next segment 0x%x found\n",
                         *(int32_t*) p_cur->p_next_segment_uid->GetBuffer() );

                /* Preload segment */
                if ( !p_next->b_preloaded )
                    p_next->Preload();

                /* Create virtual_chapter from the first edition if any */
                chapter_item_c * p_chap = ( p_next->stored_editions.size() > 0 )?( (chapter_item_c *)p_next->stored_editions[0] ) : NULL;

                 p_vchap = virtual_chapter_c::CreateVirtualChapter( p_chap, *p_next, opened_segments, tmp, b_ordered );

                if( p_vchap )
                    vchapters.push_back( p_vchap );

                p_cur = p_next;
                b_fake_ordered = true;
            }
            else /* segment not found */
                break;
        }

        /* Retime chapters */
        retimeChapters();
        if(b_fake_ordered)
            b_ordered = true;
    }

#ifdef MKV_DEBUG
    msg_Dbg( &p_main_segment.sys.demuxer, "-- RECAP-BEGIN --" );
    print();
    msg_Dbg( &p_main_segment.sys.demuxer, "-- RECAP-END --" );
#endif
}

virtual_edition_c::~virtual_edition_c()
{
    for( size_t i = 0; i < vchapters.size(); i++ )
        delete vchapters[i];
}

void virtual_edition_c::retimeSubChapters( virtual_chapter_c * p_vchap )
{
    mtime_t i_mk_stop_time = p_vchap->i_mk_virtual_stop_time;
    for( size_t i = p_vchap->sub_vchapters.size(); i-- > 0; )
    {
        virtual_chapter_c * p_vsubchap = p_vchap->sub_vchapters[i];
        //p_vsubchap->i_mk_virtual_start_time += p_vchap->i_mk_virtual_start_time;

        /*FIXME we artificially extend stop time if they were there before...*/
        /* Just for comfort*/
        p_vsubchap->i_mk_virtual_stop_time = i_mk_stop_time;
        i_mk_stop_time = p_vsubchap->i_mk_virtual_start_time;

        retimeSubChapters( p_vsubchap );
    }
}

void virtual_edition_c::retimeChapters()
{
    /* This function is just meant to be used on unordered chapters */
    if( b_ordered )
        return;

    i_duration = 0;

    /* On non ordered editions we have one top chapter == one segment */
    for( size_t i = 0; i < vchapters.size(); i++ )
    {
        virtual_chapter_c * p_vchap = vchapters[i];

        p_vchap->i_mk_virtual_start_time = i_duration;
        i_duration += p_vchap->segment.i_duration * 1000;
        p_vchap->i_mk_virtual_stop_time = i_duration;

        retimeSubChapters( p_vchap );
    }
}

virtual_segment_c::virtual_segment_c( matroska_segment_c & main_segment, std::vector<matroska_segment_c*> & p_opened_segments )
{
    /* Main segment */
    std::vector<chapter_edition_c*>::size_type i;
    i_sys_title = 0;
    p_current_vchapter = NULL;
    b_current_vchapter_entered = false;

    i_current_edition = main_segment.i_default_edition;

    for( i = 0; i < main_segment.stored_editions.size(); i++ )
    {
        /* Create a virtual edition from opened */
        virtual_edition_c * p_vedition = new virtual_edition_c( main_segment.stored_editions[i], main_segment, p_opened_segments );

        bool b_has_translate = false;
        for (size_t i=0; i < p_vedition->vchapters.size(); i++)
        {
            if ( p_vedition->vchapters[i]->segment.translations.size() != 0 )
            {
                b_has_translate = true;
                break;
            }
        }
        /* Ordered empty edition can happen when all chapters are
         * on an other segment which couldn't be found... ignore it */
        /* OK if it has chapters and the translate codec in Matroska */
        if(p_vedition->b_ordered && p_vedition->i_duration == 0 && !b_has_translate)
        {

            msg_Warn( &main_segment.sys.demuxer,
                      "Edition %s (%zu) links to other segments not found and is empty... ignoring it",
                       p_vedition->GetMainName().c_str(), i );
            if(i_current_edition == i)
            {
                msg_Warn( &main_segment.sys.demuxer,
                          "Empty edition was the default... defaulting to 0");
                i_current_edition = 0;
            }
            delete p_vedition;
        }
        else
            veditions.push_back( p_vedition );
    }
    /*if we don't have edition create a dummy one*/
    if( !main_segment.stored_editions.size() )
    {
        virtual_edition_c * p_vedition = new virtual_edition_c( NULL, main_segment, p_opened_segments );
        veditions.push_back( p_vedition );
    }

    /* Get the default edition, if there is none, use the first one */
    for( i = 0; i < veditions.size(); i++)
    {
        if( veditions[i]->p_edition && veditions[i]->p_edition->b_default )
        {
            i_current_edition = i;
            break;
        }
    }
}

virtual_segment_c::~virtual_segment_c()
{
    for( size_t i = 0; i < veditions.size(); i++ )
        delete veditions[i];
}

virtual_chapter_c *virtual_segment_c::BrowseCodecPrivate( unsigned int codec_id,
                                    bool (*match)(const chapter_codec_cmds_c &data, const void *p_cookie, size_t i_cookie_size ),
                                    const void *p_cookie,
                                    size_t i_cookie_size )
{
    virtual_edition_c * p_ved = CurrentEdition();
    if( p_ved )
        return p_ved->BrowseCodecPrivate( codec_id, match, p_cookie, i_cookie_size );

    return NULL;
}


virtual_chapter_c * virtual_edition_c::BrowseCodecPrivate( unsigned int codec_id,
                                    bool (*match)(const chapter_codec_cmds_c &data, const void *p_cookie, size_t i_cookie_size ),
                                    const void *p_cookie,
                                    size_t i_cookie_size )
{
    if( !p_edition )
        return NULL;

    for( size_t i = 0; i < vchapters.size(); i++ )
    {
        virtual_chapter_c * p_result = vchapters[i]->BrowseCodecPrivate( codec_id, match, p_cookie, i_cookie_size );
        if( p_result )
            return p_result;
    }
    return NULL;
}



virtual_chapter_c * virtual_chapter_c::BrowseCodecPrivate( unsigned int codec_id,
                                    bool (*match)(const chapter_codec_cmds_c &data, const void *p_cookie, size_t i_cookie_size ),
                                    const void *p_cookie,
                                    size_t i_cookie_size )
{
    if( !p_chapter )
        return NULL;

    if( p_chapter->BrowseCodecPrivate( codec_id, match, p_cookie, i_cookie_size ) )
        return this;

    for( size_t i = 0; i < sub_vchapters.size(); i++ )
    {
        virtual_chapter_c * p_result = sub_vchapters[i]->BrowseCodecPrivate( codec_id, match, p_cookie, i_cookie_size );
        if( p_result )
            return p_result;
    }
    return NULL;
}

bool virtual_chapter_c::ContainsTimestamp( int64_t time )
{
    /*with the current implementation only the last chapter can have a negative virtual_stop_time*/
    return ( time >= i_mk_virtual_start_time && time < i_mk_virtual_stop_time );
}

virtual_chapter_c* virtual_chapter_c::getSubChapterbyTimecode( int64_t time )
{
    for( size_t i = 0; i < sub_vchapters.size(); i++ )
    {
        if( sub_vchapters[i]->ContainsTimestamp( time ) )
            return sub_vchapters[i]->getSubChapterbyTimecode( time );
    }

    return this;
}

virtual_chapter_c* virtual_edition_c::getChapterbyTimecode( int64_t time )
{
    for( size_t i = 0; i < vchapters.size(); i++ )
    {
        if( vchapters[i]->ContainsTimestamp( time ) )
            return vchapters[i]->getSubChapterbyTimecode( time );
    }

    /* special case for the beggining of the first ordered chapter */
    if ( b_ordered && vchapters.size() )
    {
        return vchapters[0]->getSubChapterbyTimecode( time );
    }

    return NULL;
}

bool virtual_segment_c::UpdateCurrentToChapter( demux_t & demux )
{
    demux_sys_t & sys = *demux.p_sys;
    virtual_chapter_c *p_cur_vchapter = NULL;
    virtual_edition_c *p_cur_vedition = veditions[ i_current_edition ];

    bool b_has_seeked = false;

    if ( !b_current_vchapter_entered && p_current_vchapter != NULL )
    {
        b_current_vchapter_entered = true;
        if (p_current_vchapter->Enter( true ))
            return true;
    }

    if ( sys.i_pts != VLC_TS_INVALID )
    {
        if ( p_current_vchapter != NULL && p_current_vchapter->ContainsTimestamp( sys.i_pts - VLC_TS_0 ))
            p_cur_vchapter = p_current_vchapter;
        else
            p_cur_vchapter = p_cur_vedition->getChapterbyTimecode( sys.i_pts - VLC_TS_0 );
    }

    /* we have moved to a new chapter */
    if ( p_cur_vchapter != NULL && p_current_vchapter != p_cur_vchapter )
        {
            msg_Dbg( &demux, "NEW CHAPTER %" PRId64 " uid=%" PRId64, sys.i_pts - VLC_TS_0, p_cur_vchapter->p_chapter->i_uid );
            if ( p_cur_vedition->b_ordered )
            {
                /* FIXME EnterAndLeave has probably been broken for a long time */
                // Leave/Enter up to the link point
                b_has_seeked = p_cur_vchapter->EnterAndLeave( p_current_vchapter );
                if ( !b_has_seeked )
                {
                    // only physically seek if necessary
                    if ( p_current_vchapter == NULL ||
                        ( p_current_vchapter && &p_current_vchapter->segment != &p_cur_vchapter->segment ) ||
                        ( p_current_vchapter->p_chapter->i_end_time != p_cur_vchapter->p_chapter->i_start_time ))
                    {
                        /* Forcing reset pcr */
                        es_out_Control( demux.out, ES_OUT_RESET_PCR);
                        Seek( demux, p_cur_vchapter->i_mk_virtual_start_time, p_cur_vchapter, -1 );
                        return true;
                    }
                    sys.i_start_pts = p_cur_vchapter->i_mk_virtual_start_time + VLC_TS_0;
                }
            }

            p_current_vchapter = p_cur_vchapter;
            if ( p_cur_vchapter->i_seekpoint_num > 0 )
            {
                demux.info.i_update |= INPUT_UPDATE_TITLE | INPUT_UPDATE_SEEKPOINT;
                demux.info.i_title = sys.i_current_title = i_sys_title;
                demux.info.i_seekpoint = p_cur_vchapter->i_seekpoint_num - 1;
            }

            return b_has_seeked;
        }
        else if ( p_cur_vchapter == NULL )
        {
            /* out of the scope of the data described by chapters, leave the edition */
            if ( p_cur_vedition->b_ordered && p_current_vchapter != NULL )
            {
                if ( !p_current_vchapter->Leave( ) )
                {
                    p_current_vchapter = NULL;
                    b_current_vchapter_entered = false;
                }
                else
                    return true;
            }
        }
    return false;
}

bool virtual_chapter_c::Leave( )
{
    if( !p_chapter )
        return false;

    return p_chapter->Leave( true );
}

bool virtual_chapter_c::EnterAndLeave( virtual_chapter_c *p_leaving_vchapter, bool b_enter )
{
    if( !p_chapter )
        return false;

    return p_chapter->EnterAndLeave( p_leaving_vchapter->p_chapter, b_enter );
}

void virtual_segment_c::Seek( demux_t & demuxer, mtime_t i_mk_date,
                              virtual_chapter_c *p_vchapter, int64_t i_global_position )
{
    demux_sys_t *p_sys = demuxer.p_sys;


    /* find the actual time for an ordered edition */
    if ( p_vchapter == NULL )
        /* 1st, we need to know in which chapter we are */
        p_vchapter = veditions[ i_current_edition ]->getChapterbyTimecode( i_mk_date );

    if ( p_vchapter != NULL )
    {
        mtime_t i_mk_time_offset = p_vchapter->i_mk_virtual_start_time - ( ( p_vchapter->p_chapter )? p_vchapter->p_chapter->i_start_time : 0 );
        p_sys->i_mk_chapter_time = i_mk_time_offset - p_vchapter->segment.i_mk_start_time /* + VLC_TS_0 */;
        if ( p_vchapter->p_chapter && p_vchapter->i_seekpoint_num > 0 )
        {
            demuxer.info.i_update |= INPUT_UPDATE_TITLE | INPUT_UPDATE_SEEKPOINT;
            demuxer.info.i_title = p_sys->i_current_title = i_sys_title;
            demuxer.info.i_seekpoint = p_vchapter->i_seekpoint_num - 1;
        }

        if( p_current_vchapter == NULL || &p_current_vchapter->segment != &p_vchapter->segment )
        {
            if ( p_current_vchapter )
            {
                KeepTrackSelection( p_current_vchapter->segment, p_vchapter->segment );
                p_current_vchapter->segment.UnSelect();
            }
            msg_Dbg( &demuxer, "SWITCH CHAPTER uid=%" PRId64, p_vchapter->p_chapter ? p_vchapter->p_chapter->i_uid : 0 );
            p_current_vchapter = p_vchapter;
            p_sys->PreparePlayback( *this, i_mk_date );
        }
        else
        {
            p_current_vchapter = p_vchapter;
            p_current_vchapter->segment.Seek( i_mk_date, i_mk_time_offset, i_global_position );
        }
    }
}

virtual_chapter_c * virtual_chapter_c::FindChapter( int64_t i_find_uid )
{
    if( p_chapter && ( p_chapter->i_uid == i_find_uid ) )
        return this;

    for( size_t i = 0; i < sub_vchapters.size(); i++ )
    {
        virtual_chapter_c * p_res = sub_vchapters[i]->FindChapter( i_find_uid );
        if( p_res )
            return p_res;
    }

    return NULL;
}

virtual_chapter_c * virtual_segment_c::FindChapter( int64_t i_find_uid )
{
    virtual_edition_c * p_edition = veditions[i_current_edition];

    for( size_t i = 0; i < p_edition->vchapters.size(); i++ )
    {
        virtual_chapter_c * p_chapter = p_edition->vchapters[i]->FindChapter( i_find_uid );
        if( p_chapter )
            return p_chapter;
    }
    return NULL;
}

int virtual_chapter_c::PublishChapters( input_title_t & title, int & i_user_chapters, int i_level )
{
    if ( p_chapter && ( !p_chapter->b_display_seekpoint || p_chapter->psz_name == "" ) )
    {
        p_chapter->psz_name = p_chapter->GetCodecName();
        if ( p_chapter->psz_name != "" )
            p_chapter->b_display_seekpoint = true;
    }

    if ( ( p_chapter && p_chapter->b_display_seekpoint &&
         ( ( sub_vchapters.size() > 0 && i_mk_virtual_start_time != sub_vchapters[0]->i_mk_virtual_start_time) ||
           sub_vchapters.size() == 0 ) ) || !p_chapter )
    {
        seekpoint_t *sk = vlc_seekpoint_New();

        sk->i_time_offset = i_mk_virtual_start_time;
        if( p_chapter )
            sk->psz_name = strdup( p_chapter->psz_name.c_str() );
        else
            sk->psz_name = strdup("dummy chapter");

        /* A start time of '0' is ok. A missing ChapterTime element is ok, too, because '0' is its default value. */
        title.i_seekpoint++;
        title.seekpoint = (seekpoint_t**)xrealloc( title.seekpoint,
                                 title.i_seekpoint * sizeof( seekpoint_t* ) );
        title.seekpoint[title.i_seekpoint-1] = sk;

        if ( (p_chapter && p_chapter->b_user_display ) ||  !p_chapter )
            i_user_chapters++;
    }
    i_seekpoint_num = i_user_chapters;

    for( size_t i = 0; i < sub_vchapters.size(); i++ )
        sub_vchapters[i]->PublishChapters( title, i_user_chapters, i_level + 1 );

    return i_user_chapters;
}


int virtual_edition_c::PublishChapters( input_title_t & title, int & i_user_chapters, int i_level )
{

    /* HACK for now don't expose edition as a seekpoint if its start time is the same than it's first chapter */
    if( vchapters.size() > 0 &&
        vchapters[0]->i_mk_virtual_start_time && p_edition )
    {
        seekpoint_t *sk = vlc_seekpoint_New();

        sk->i_time_offset = 0;
        sk->psz_name = strdup( p_edition->psz_name.c_str() );

        title.i_seekpoint++;
        title.seekpoint = static_cast<seekpoint_t**>( xrealloc( title.seekpoint,
                             title.i_seekpoint * sizeof( seekpoint_t* ) ) );
        title.seekpoint[title.i_seekpoint - 1] = sk;
        i_level++;

        i_user_chapters++;
        i_seekpoint_num = i_user_chapters;
    }

//    if( chapters.size() > 1 )
        for( size_t i = 0; i < vchapters.size(); i++ )
            vchapters[i]->PublishChapters( title, i_user_chapters, i_level );

    return i_user_chapters;
}

std::string virtual_edition_c::GetMainName()
{
    if( p_edition )
        return p_edition->GetMainName();

    return std::string("");
}

bool virtual_chapter_c::Enter( bool b_do_subs )
{
    if( p_chapter )
        return p_chapter->Enter( b_do_subs );
    return false;
}

bool virtual_chapter_c::Leave( bool b_do_subs )
{
    if( p_chapter )
        return p_chapter->Leave( b_do_subs );
    return false;
}

#ifdef MKV_DEBUG
void virtual_chapter_c::print()
{
    msg_Dbg( &p_segment->sys.demuxer, "*** chapter %" PRId64 " - %" PRId64 " (%u)",
             i_mk_virtual_start_time, i_mk_virtual_stop_time, sub_chapters.size() );
    for( size_t i = 0; i < sub_chapters.size(); i++ )
        sub_chapters[i]->print();
}
#endif

void virtual_segment_c::KeepTrackSelection( matroska_segment_c & old, matroska_segment_c & next )
{
    size_t i, j;
    char *sub_lang = NULL, *aud_lang = NULL;
    /* get the current ES language selection */
    for( i = 0; i < old.tracks.size(); i++)
    {
        mkv_track_t *p_tk = old.tracks[i];
        es_format_t *p_ofmt = &p_tk->fmt;
        if( p_tk->p_es )
        {
            bool state = false;
            es_out_Control( old.sys.demuxer.out, ES_OUT_GET_ES_STATE, p_tk->p_es, &state );
            if( state )
            {
                if( p_ofmt->i_cat == AUDIO_ES )
                    aud_lang = p_tk->fmt.psz_language;
                else if( p_ofmt->i_cat == SPU_ES )
                    sub_lang = p_tk->fmt.psz_language;
            }
        }
    }
    for( i = 0; i < next.tracks.size(); i++)
    {
        mkv_track_t *p_tk = next.tracks[i];
        es_format_t *p_nfmt = &p_tk->fmt;

        /* Let's only do that for audio and video for now */
        if( p_nfmt->i_cat == AUDIO_ES || p_nfmt->i_cat == VIDEO_ES )
        {

            /* check for a similar elementary stream */
            for( j = 0; j < old.tracks.size(); j++)
            {
                es_format_t * p_ofmt = &old.tracks[j]->fmt;

                if( !old.tracks[j]->p_es )
                    continue;

                if( ( p_nfmt->i_cat == p_ofmt->i_cat ) &&
                    ( p_nfmt->i_codec == p_ofmt->i_codec ) &&
                    ( p_nfmt->i_priority == p_ofmt->i_priority ) &&
                    ( p_nfmt->i_bitrate == p_ofmt->i_bitrate ) &&
                    ( p_nfmt->i_extra == p_ofmt->i_extra ) &&
                    ( p_nfmt->i_extra == 0 ||
                      !memcmp( p_nfmt->p_extra, p_ofmt->p_extra, p_nfmt->i_extra ) ) &&
                    !strcasecmp( p_nfmt->psz_language, p_ofmt->psz_language ) &&
                    ( ( p_nfmt->i_cat == AUDIO_ES &&
                        !memcmp( &p_nfmt->audio, &p_ofmt->audio, sizeof(audio_format_t) ) ) ||
                      ( p_nfmt->i_cat == VIDEO_ES &&
                        !memcmp( &p_nfmt->video, &p_ofmt->video, sizeof(video_format_t) ) ) ) )
                {
                    /* FIXME handle video palettes... */
                    msg_Warn( &old.sys.demuxer, "Reusing decoder of old track %zu for track %zu", j, i);
                    p_tk->p_es = old.tracks[j]->p_es;
                    old.tracks[j]->p_es = NULL;
                    break;
                }
            }
        }
        p_tk->fmt.i_priority &= ~(0x10);
        if( ( sub_lang && p_nfmt->i_cat == SPU_ES && !strcasecmp(sub_lang, p_nfmt->psz_language) ) ||
            ( aud_lang && p_nfmt->i_cat == AUDIO_ES && !strcasecmp(aud_lang, p_nfmt->psz_language) ) )
        {
            msg_Warn( &old.sys.demuxer, "Since previous segment used lang %s forcing track %zu",
                      p_nfmt->psz_language, i);
            p_tk->fmt.i_priority |= 0x10;
            p_tk->b_forced = true;
        }
    }
}
