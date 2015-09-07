/*
 FLIF - Free Lossless Image Format
 Copyright (C) 2010-2015  Jon Sneyers & Pieter Wuille

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

/*
 Parts of this code are based on code from the FFMPEG project, in
 particular:
 - ffv1.c - Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
 - common.h - copyright (c) 2006 Michael Niedermayer <michaelni@gmx.at>
 - rangecoder.c - Copyright (c) 2004 Michael Niedermayer <michaelni@gmx.at>
*/

#include <string>

#include "maniac/rac.h"
#include "maniac/compound.h"
#include "maniac/util.h"

#include "image/image.h"
#include "image/color_range.h"
#include "transform/factory.h"

#include "flif_config.h"



FILE *f;  // the compressed file

std::vector<ColorVal> grey; // a pixel with values in the middle of the bounds

typedef SimpleBitChance                         FLIFBitChancePass1;

// faster:
//typedef SimpleBitChance                         FLIFBitChancePass2;
//typedef SimpleBitChance                         FLIFBitChanceParities;
//typedef SimpleBitChance                         FLIFBitChanceMeta;

// better compression:
typedef MultiscaleBitChance<6,SimpleBitChance>  FLIFBitChancePass2;
typedef MultiscaleBitChance<6,SimpleBitChance>  FLIFBitChanceParities;
typedef MultiscaleBitChance<6,SimpleBitChance>  FLIFBitChanceMeta;

typedef MultiscaleBitChance<6,SimpleBitChance>  FLIFBitChanceTree;

template<typename RAC> void static write_name(RAC& rac, std::string str)
{
    UniformSymbolCoder<RAC> coder(rac);
    coder.write_int(3, 8, str.size());
    for (unsigned int i=0; i<str.size(); i++) {
        char c = str[i];
        int n = ((c >= 'A' && c <= 'Z') ? c - 'A' :
                ((c >= 'a' && c <= 'z') ? c - 'a' :
                ((c >= '0' && c <= '9') ? c - '0' + 26 : 36)));
        coder.write_int(0, 36, n);
    }
}

template<typename RAC> std::string static read_name(RAC& rac)
{
    static char cs[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
    UniformSymbolCoder<RAC> coder(rac);
    int l = coder.read_int(3, 8);
    std::string str;
    for (int i=0; i<l; i++) {
        int n = coder.read_int(0, 36);
        str += cs[n];
    }
    return str;
}


// planes:
// 0    Y channel (luminance)
// 1    I (chroma)
// 2    Q (chroma)
// 3    Alpha (transparency)


/******************************************/
/*   scanlines encoding/decoding          */
/******************************************/


const int NB_PROPERTIES_scanlines[] = {7,8,9,7};
const int NB_PROPERTIES_scanlinesA[] = {8,9,10,7};
void static initPropRanges_scanlines(Ranges &propRanges, const ColorRanges &ranges, int p)
{
    propRanges.clear();
    int min = ranges.min(p);
    int max = ranges.max(p);
    int mind = min - max, maxd = max - min;

    if (p != 3) {
      for (int pp = 0; pp < p; pp++) {
        propRanges.push_back(std::make_pair(ranges.min(pp), ranges.max(pp)));  // pixels on previous planes
      }
      if (ranges.numPlanes()>3) propRanges.push_back(std::make_pair(ranges.min(3), ranges.max(3)));  // pixel on alpha plane
    }
    propRanges.push_back(std::make_pair(min,max));   // guess (median of 3)
    propRanges.push_back(std::make_pair(0,3));       // which predictor was it
    propRanges.push_back(std::make_pair(mind,maxd));
    propRanges.push_back(std::make_pair(mind,maxd));
    propRanges.push_back(std::make_pair(mind,maxd));
    propRanges.push_back(std::make_pair(mind,maxd));
    propRanges.push_back(std::make_pair(mind,maxd));
}



ColorVal predict_and_calcProps_scanlines(Properties &properties, const ColorRanges *ranges, const Image &image, const int p, const int r, const int c, ColorVal &min, ColorVal &max) {
    ColorVal guess;
    int which = 0;
    int index=0;
    if (p != 3) {
      for (int pp = 0; pp < p; pp++) {
        properties[index++] = image(pp,r,c);
      }
      if (image.numPlanes()>3) properties[index++] = image(3,r,c);
    }
    ColorVal left = (c>0 ? image(p,r,c-1) : grey[p]);;
    ColorVal top = (r>0 ? image(p,r-1,c) : grey[p]);
    ColorVal topleft = (r>0 && c>0 ? image(p,r-1,c-1) : grey[p]);
    ColorVal gradientTL = left + top - topleft;
    guess = median3(gradientTL, left, top);
    ranges->snap(p,properties,min,max,guess);
    if (guess == gradientTL) which = 0;
    else if (guess == left) which = 1;
    else if (guess == top) which = 2;

    properties[index++] = guess;
    properties[index++] = which;

    if (c > 0 && r > 0) { properties[index++] = left - topleft; properties[index++] = topleft - top; }
                 else   { properties[index++] = 0; properties[index++] = 0;  }

    if (c+1 < image.cols() && r > 0) properties[index++] = top - image(p,r-1,c+1); // top - topright
                 else   properties[index++] = 0;
    if (r > 1) properties[index++] = image(p,r-2,c)-top;    // toptop - top
         else properties[index++] = 0;
    if (c > 1) properties[index++] = image(p,r,c-2)-left;    // leftleft - left
         else properties[index++] = 0;
    return guess;
}

template<typename Coder> void encode_scanlines_inner(std::vector<Coder*> &coders, const Image &image, const ColorRanges *ranges)
{
    ColorVal min,max;
    long fs = ftell(f);
    long pixels = image.cols()*image.rows();
    int nump = image.numPlanes();
    int beginp = (nump>3 ? 3 : 0); int i=0;
    for (int p = beginp; i++ < nump; p = (p+1)%nump) {
        Properties properties((nump>3?NB_PROPERTIES_scanlinesA[p]:NB_PROPERTIES_scanlines[p]));
        if (ranges->min(p) < ranges->max(p))
          fprintf(stdout,"[%i] ENC_scanlines ",p);
        else continue;
        fflush(stdout);
        for (int r = 0; r < image.rows(); r++) {
            for (int c = 0; c < image.cols(); c++) {
                if (nump>3 && p<3 && image(3,r,c) == 0) continue;
                ColorVal guess = predict_and_calcProps_scanlines(properties,ranges,image,p,r,c,min,max);
                ColorVal curr = image(p,r,c);
                coders[p]->write_int(properties, min - guess, max - guess, curr - guess);
            }
        }
        long nfs = ftell(f);
        if (nfs-fs > 0) fprintf(stdout,"\tfilesize : %li (+%li for %li pixels, %f bpp)\n", nfs, nfs-fs, pixels, 8.0*(nfs-fs)/pixels );
        fs = nfs;
    }
}

template<typename Rac, typename Coder> void encode_scanlines_pass(Rac &rac, const Image &image, const ColorRanges *ranges, std::vector<Tree> &forest, int repeats)
{
    std::vector<Coder*> coders;
    for (int p = 0; p < ranges->numPlanes(); p++) {
        Ranges propRanges;
        initPropRanges_scanlines(propRanges, *ranges, p);
        coders.push_back(new Coder(rac, propRanges, forest[p]));
    }

    if (repeats>1) printf("Iterating %i times to find a better tree.\n",repeats);
    while(repeats-- > 0) {
     encode_scanlines_inner(coders, image, ranges);
    }

    for (int p = 0; p < image.numPlanes(); p++) {
        coders[p]->simplify();
    }

    for (int p = 0; p < image.numPlanes(); p++) {
#ifdef STATS
        indent(0); printf("Plane %i\n", p);
        coders[p]->info(0+1);
#endif
        delete coders[p];
    }
}


void encode_scanlines_interpol_zero_alpha(Image &image, const ColorRanges *ranges)
{

    ColorVal min,max;
    int nump = image.numPlanes();
    printf("Replacing fully transparent pixels with predicted pixel values at the other planes\n");
    if (nump > 3)
    for (int p = 0; p < 3; p++) {
        Properties properties((nump>3?NB_PROPERTIES_scanlinesA[p]:NB_PROPERTIES_scanlines[p]));
        if (ranges->min(p) < ranges->max(p))
          fprintf(stdout,"[%i] interpol_zero_alpha ",p);
        else continue;
        fflush(stdout);
        for (int r = 0; r < image.rows(); r++) {
            for (int c = 0; c < image.cols(); c++) {
                if (image(3,r,c) == 0) {
                    image(p,r,c) = predict_and_calcProps_scanlines(properties,ranges,image,p,r,c,min,max);
                }
            }
        }
    }
    printf("\n");
}


template<typename Coder> void decode_scanlines_inner(std::vector<Coder*> &coders, Image &image, const ColorRanges *ranges)
{

    ColorVal min,max;
    int nump = image.numPlanes();
    int beginp = (nump>3 ? 3 : 0); int i=0;
    for (int p = beginp; i++ < nump; p = (p+1)%nump) {
        Properties properties((nump>3?NB_PROPERTIES_scanlinesA[p]:NB_PROPERTIES_scanlines[p]));
        if (ranges->min(p) < ranges->max(p))
          fprintf(stdout,"[%i] DEC_scanlines ",p);
        else continue;
        for (int r = 0; r < image.rows(); r++) {
            for (int c = 0; c < image.cols(); c++) {
                ColorVal guess = predict_and_calcProps_scanlines(properties,ranges,image,p,r,c,min,max);
                if (nump>3 && p<3 && image(3,r,c) == 0) { image(p,r,c)=guess; continue;}
                ColorVal curr = coders[p]->read_int(properties, min - guess, max - guess) + guess;
                image(p,r,c) = curr;
            }
        }
    }
}

template<typename Rac, typename Coder> void decode_scanlines_pass(Rac &rac, Image &image, const ColorRanges *ranges, std::vector<Tree> &forest)
{
    std::vector<Coder*> coders;
    for (int p = 0; p < image.numPlanes(); p++) {
        Ranges propRanges;
        initPropRanges_scanlines(propRanges, *ranges, p);
        coders.push_back(new Coder(rac, propRanges, forest[p]));
    }
    decode_scanlines_inner(coders, image, ranges);
    for (int p = 0; p < image.numPlanes(); p++) {
        delete coders[p];
    }
}




/******************************************/
/*   FLIF2 encoding/decoding              */
/******************************************/
const int NB_PROPERTIES[] = {8,7,8,8};
const int NB_PROPERTIESA[] = {9,8,9,8};
void static initPropRanges(Ranges &propRanges, const ColorRanges &ranges, int p)
{
    propRanges.clear();
    int min = ranges.min(p);
    int max = ranges.max(p);
    int mind = min - max, maxd = max - min;
    if (p != 3) {       // alpha channel first
      for (int pp = 0; pp < p; pp++) {
        propRanges.push_back(std::make_pair(ranges.min(pp), ranges.max(pp)));  // pixels on previous planes
      }
      if (ranges.numPlanes()>3) propRanges.push_back(std::make_pair(ranges.min(3), ranges.max(3)));  // pixel on alpha plane
    }
    propRanges.push_back(std::make_pair(mind,maxd)); // neighbor A - neighbor B   (top-bottom or left-right)
    propRanges.push_back(std::make_pair(min,max));   // guess (median of 3)
    propRanges.push_back(std::make_pair(0,3));       // which predictor was it
    propRanges.push_back(std::make_pair(mind,maxd));
    propRanges.push_back(std::make_pair(mind,maxd));
    propRanges.push_back(std::make_pair(mind,maxd));

    if (p == 0 || p == 3) {
      propRanges.push_back(std::make_pair(mind,maxd));
      propRanges.push_back(std::make_pair(mind,maxd));
    }
}

// Prediction used for interpolation. Does not have to be the same as the guess used for encoding/decoding.
inline ColorVal predict(const Image &image, int z, int p, int r, int c)
{
    if (z%2 == 0) { // filling horizontal lines
      ColorVal top = image(p,z,r-1,c);
      ColorVal bottom = (r+1 < image.rows(z) ? image(p,z,r+1,c) : top); //grey[p]);
      ColorVal avg = (top + bottom)/2;
      return avg;
    } else { // filling vertical lines
      ColorVal left = image(p,z,r,c-1);
      ColorVal right = (c+1 < image.cols(z) ? image(p,z,r,c+1) : left); //grey[p]);
      ColorVal avg = (left + right)/2;
      return avg;
    }
}

// Actual prediction. Also sets properties. Property vector should already have the right size before calling this.
ColorVal predict_and_calcProps(Properties &properties, const ColorRanges *ranges, const Image &image, const int z, const int p, const int r, const int c, ColorVal &min, ColorVal &max) {
    ColorVal guess;
    int which = 0;
    int index = 0;

    if (p != 3) {
    for (int pp = 0; pp < p; pp++) {
        properties[index++] = image(pp,z,r,c);
    }
    if (image.numPlanes()>3) properties[index++] = image(3,z,r,c);
    }
    ColorVal left;
    ColorVal top;
    ColorVal topleft = (r>0 && c>0 ? image(p,z,r-1,c-1) : grey[p]);
    ColorVal topright = (r>0 && c+1 < image.cols(z) ? image(p,z,r-1,c+1) : grey[p]);
    ColorVal bottomleft = (r+1 < image.rows(z) && c>0 ? image(p,z,r+1,c-1) : grey[p]);
    if (z%2 == 0) { // filling horizontal lines
      left = (c>0 ? image(p,z,r,c-1) : grey[p]);
      top = image(p,z,r-1,c);
      ColorVal gradientTL = left + top - topleft;
      ColorVal bottom = (r+1 < image.rows(z) ? image(p,z,r+1,c) : top); //grey[p]);
      ColorVal gradientBL = left + bottom - bottomleft;
      ColorVal avg = (top + bottom)/2;
      guess = median3(gradientTL, gradientBL, avg);
      ranges->snap(p,properties,min,max,guess);
      if (guess == avg) which = 0;
      else if (guess == gradientTL) which = 1;
      else if (guess == gradientBL) which = 2;
      properties[index++] = top-bottom;

    } else { // filling vertical lines
      left = image(p,z,r,c-1);
      top = (r>0 ? image(p,z,r-1,c) : grey[p]);
      ColorVal gradientTL = left + top - topleft;
      ColorVal right = (c+1 < image.cols(z) ? image(p,z,r,c+1) : left); //grey[p]);
      ColorVal gradientTR = right + top - topright;
      ColorVal avg = (left + right      )/2;
      guess = median3(gradientTL, gradientTR, avg);
      ranges->snap(p,properties,min,max,guess);
      if (guess == avg) which = 0;
      else if (guess == gradientTL) which = 1;
      else if (guess == gradientTR) which = 2;
      properties[index++] = left-right;
    }
    properties[index++]=guess;
    properties[index++]=which;

    if (c > 0 && r > 0) { properties[index++]=left - topleft; properties[index++]=topleft - top; }
                 else   { properties[index++]=0; properties[index++]=0; }

    if (c+1 < image.cols(z) && r > 0) properties[index++]=top - topright;
                 else   properties[index++]=0;

    if (p == 0 || p == 3) {
     if (r > 1) properties[index++]=image(p,z,r-2,c)-top;    // toptop - top
         else properties[index++]=0;
     if (c > 1) properties[index++]=image(p,z,r,c-2)-left;    // leftleft - left
         else properties[index++]=0;
    }
    return guess;
}

int plane_zoomlevels(const Image &image, const int beginZL, const int endZL) {
    return image.numPlanes() * (beginZL - endZL + 1);
}

std::pair<int, int> plane_zoomlevel(const Image &image, const int beginZL, const int endZL, int i) {
    assert(i >= 0);
    assert(i < plane_zoomlevels(image, beginZL, endZL));
    // simple order: interleave planes, zoom in
//    int p = i % image.numPlanes();
//    int zl = beginZL - (i / image.numPlanes());

    // more advanced order: give priority to more important plane(s)
    // assumption: plane 0 is Y, plane 1 is I, plane 2 is Q, plane 3 is perhaps alpha, next planes (not used at the moment) are not important
    const int max_behind[] = {0, 2, 4, 0, 16, 18, 20, 22};
    int np = image.numPlanes();
    if (np>7) {
      // too many planes, do something simple
      int p = i % image.numPlanes();
      int zl = beginZL - (i / image.numPlanes());
      return std::pair<int, int>(p,zl);
    }
    std::vector<int> czl(np);
    for (int &pzl : czl) pzl = beginZL+1;
    int highest_priority_plane = 0;
    if (np >= 4) highest_priority_plane = 3; // alpha first
    int nextp = highest_priority_plane;
    while (i >= 0) {
      czl[nextp]--;
      i--;
      if (i<0) break;
      nextp=highest_priority_plane;
      for (int p=0; p<np; p++) {
        if (czl[p] > czl[highest_priority_plane] + max_behind[p]) {
          nextp = p; break;
        }
      }
      // ensure that nextp is not at the most detailed zoomlevel yet
      while (czl[nextp] <= endZL) nextp = (nextp+1)%np;
    }
    int p = nextp;
    int zl = czl[p];

    return std::pair<int, int>(p,zl);
}

template<typename Coder, typename ParityCoder> void encode_FLIF2_inner(std::vector<Coder*> &coders, ParityCoder &parityCoder, const Image &image, const ColorRanges *ranges, const int beginZL, const int endZL)
{
    long fs = ftell(f);
    ColorVal min,max;
    int nump = image.numPlanes();
    for (int i = 0; i < plane_zoomlevels(image, beginZL, endZL); i++) {
      std::pair<int, int> pzl = plane_zoomlevel(image, beginZL, endZL, i);
      int p = pzl.first;
      int z = pzl.second;
      if (ranges->min(p) < ranges->max(p))
        fprintf(stdout,"[%i/%i] ENC(p:%i,z:%i), %ix%i\t",i,plane_zoomlevels(image, beginZL, endZL)-1,p,z,image.rows(z),image.cols(z));
      else continue;
      fflush(stdout);
      Properties properties((nump>3?NB_PROPERTIESA[p]:NB_PROPERTIES[p]));
      if (z % 2 == 0) {
        // horizontal: scan the odd rows, output pixel values
          for (int r = 1; r < image.rows(z); r += 2) {
            for (int c = 0; c < image.cols(z); c++) {
                    if (nump>3 && p<3 && image(3,z,r,c) == 0) continue;
                    ColorVal guess = predict_and_calcProps(properties,ranges,image,z,p,r,c,min,max);
                    ColorVal curr = image(p,z,r,c);
                    assert (curr <= max); assert (curr >= min);
                    coders[p]->write_int(properties, min - guess, max - guess, curr - guess);
            }
          }
      } else {
        // vertical: scan the odd columns
          for (int r = 0; r < image.rows(z); r++) {
            for (int c = 1; c < image.cols(z); c += 2) {
                    if (nump>3 && p<3 && image(3,z,r,c) == 0) continue;
                    ColorVal guess = predict_and_calcProps(properties,ranges,image,z,p,r,c,min,max);
                    ColorVal curr = image(p,z,r,c);
                    assert (curr <= max); assert (curr >= min);
                    coders[p]->write_int(properties, min - guess, max - guess, curr - guess);
            }
          }
      }
      long nfs = ftell(f);
      long pixels = image.cols(z)*image.rows(z)/2;
      if (nfs-fs > 0) fprintf(stdout,"filesize:%li (+%li bytes, %li pixels, %f bpp)\n", nfs, nfs-fs, pixels, 8.0*(nfs-fs)/pixels );
      fs = nfs;
    }
}

template<typename Rac, typename Coder> void encode_FLIF2_pass(Rac &rac, const Image &image, const ColorRanges *ranges, std::vector<Tree> &forest, const int beginZL, const int endZL, int repeats)
{
    std::vector<Coder*> coders;
    for (int p = 0; p < ranges->numPlanes(); p++) {
        Ranges propRanges;
        initPropRanges(propRanges, *ranges, p);
        coders.push_back(new Coder(rac, propRanges, forest[p]));
    }

    if (beginZL == image.zooms()) {
      // special case: very left top pixel must be written first to get it all started
      SimpleSymbolCoder<FLIFBitChanceMeta, Rac, 24> metaCoder(rac);
      for (int p = 0; p < image.numPlanes(); p++) {
        ColorVal curr = image(p,0,0);
        metaCoder.write_int(ranges->min(p), ranges->max(p), curr);
      }
    }
    SimpleBitCoder<FLIFBitChanceParities,Rac> parityCoder(rac);

    if (repeats>1) printf("Iterating %i times to find a better tree.\n",repeats);
    while(repeats-- > 0) {
     encode_FLIF2_inner(coders, parityCoder, image, ranges, beginZL, endZL);
    }

    for (int p = 0; p < image.numPlanes(); p++) {
        coders[p]->simplify();
    }

    for (int p = 0; p < image.numPlanes(); p++) {
#ifdef STATS
        indent(0); printf("Plane %i\n", p);
        coders[p]->info(0+1);
#endif
        delete coders[p];
    }
}

void encode_FLIF2_interpol_zero_alpha(Image &image, const ColorRanges *ranges, const int beginZL, const int endZL)
{
    printf("Replacing fully transparent pixels with predicted pixel values at the other planes\n");
    for (int i = 0; i < plane_zoomlevels(image, beginZL, endZL); i++) {
      std::pair<int, int> pzl = plane_zoomlevel(image, beginZL, endZL, i);
      int p = pzl.first;
      int z = pzl.second;
      if (p == 3) continue;
      fprintf(stdout,"[%i] interpol_zero_alpha ",p);
      fflush(stdout);
      if (z % 2 == 0) {
        // horizontal: scan the odd rows
          for (int r = 1; r < image.rows(z); r += 2) {
            for (int c = 0; c < image.cols(z); c++) {
               if (image(3,z,r,c) == 0) image(p,z,r,c) = predict(image,z,p,r,c);
            }
          }
      } else {
        // vertical: scan the odd columns
          for (int r = 0; r < image.rows(z); r++) {
            for (int c = 1; c < image.cols(z); c += 2) {
               if (image(3,z,r,c) == 0) image(p,z,r,c) = predict(image,z,p,r,c);
            }
          }
      }
    }
    fprintf(stdout,"\n");
}


// interpolate rest of the image
// used when decoding lossy
void decode_FLIF2_inner_interpol(Image &image, const ColorRanges *ranges, int I, const int beginZL, const int endZL, int R)
{
    for (int i = I; i < plane_zoomlevels(image, beginZL, endZL); i++) {
      std::pair<int, int> pzl = plane_zoomlevel(image, beginZL, endZL, i);
      int p = pzl.first;
      int z = pzl.second;
      fprintf(stdout,"[%i/%i] INTERPOLATE Plane %i, Zoomlevel %i, size: %ix%i\n",i,plane_zoomlevels(image, beginZL, endZL)-1,p,z,image.rows(z),image.cols(z));
      if (z % 2 == 0) {
        // horizontal: scan the odd rows
          for (int r = (I==i?R:1); r < image.rows(z); r += 2) {
            for (int c = 0; c < image.cols(z); c++) {
               image(p,z,r,c) = predict(image,z,p,r,c);
//               image(p,z,r,c) = image(p,z,r-1,c);
            }
          }
      } else {
        // vertical: scan the odd columns
          for (int r = (I==i?R:0); r < image.rows(z); r++) {
            for (int c = 1; c < image.cols(z); c += 2) {
               image(p,z,r,c) = predict(image,z,p,r,c);
//               image(p,z,r,c) = image(p,z,r,c-1);
//               image(p,z,r,c) = ranges->snap(p,predict(image,z,p,r,c),z,r,c);
            }
          }
      }
    }
}

template<typename Coder, typename ParityCoder> void decode_FLIF2_inner(std::vector<Coder*> &coders, ParityCoder &parityCoder, Image &image, const ColorRanges *ranges, const int beginZL, const int endZL, int lastI)
{
    ColorVal min,max;
    int nump = image.numPlanes();
    // decode
    for (int i = 0; i < plane_zoomlevels(image, beginZL, endZL); i++) {
      std::pair<int, int> pzl = plane_zoomlevel(image, beginZL, endZL, i);
      int p = pzl.first;
      int z = pzl.second;
      if (lastI != -1  && i > lastI) {
              decode_FLIF2_inner_interpol(image, ranges, i, beginZL, endZL, (z%2 == 0 ?1:0));
              return;
      }
      if (ranges->min(p) < ranges->max(p))
        fprintf(stdout,"[%i/%i] DEC Plane %i, Zoomlevel %i, size: %ix%i\n",i,plane_zoomlevels(image, beginZL, endZL)-1,p,z,image.rows(z),image.cols(z));
      else continue;
      ColorVal curr;
      Properties properties((nump>3?NB_PROPERTIESA[p]:NB_PROPERTIES[p]));
      if (z % 2 == 0) {
          for (int r = 1; r < image.rows(z); r += 2) {
#ifdef CHECK_FOR_BROKENFILES
            if (feof(f)) {
              printf("Row %i: Unexpected file end. Interpolation from now on.\n",r);
              decode_FLIF2_inner_interpol(image, ranges, i, beginZL, endZL, (r>1?r-2:r));
              return;
            }
#endif
            for (int c = 0; c < image.cols(z); c++) {
                     if (nump>3 && p<3 && image(3,z,r,c) == 0) {image(p,z,r,c) = predict(image,z,p,r,c); continue;}
                     ColorVal guess = predict_and_calcProps(properties,ranges,image,z,p,r,c,min,max);
                     curr = coders[p]->read_int(properties, min - guess, max - guess) + guess;
                     image(p,z,r,c) = curr;
            }
        }
      } else {
          for (int r = 0; r < image.rows(z); r++) {
#ifdef CHECK_FOR_BROKENFILES
            if (feof(f)) {
              printf("Row %i: Unexpected file end. Interpolation from now on.\n", r);
              decode_FLIF2_inner_interpol(image, ranges, i, beginZL, endZL, (r>0?r-1:r));
              return;
            }
#endif
            for (int c = 1; c < image.cols(z); c += 2) {
                     if (nump>3 && p<3 && image(3,z,r,c) == 0) {image(p,z,r,c) = predict(image,z,p,r,c); continue;}
                     ColorVal guess = predict_and_calcProps(properties,ranges,image,z,p,r,c,min,max);
                     curr = coders[p]->read_int(properties, min - guess, max - guess) + guess;
                     image(p,z,r,c) = curr;
            }
        }
      }
    }
}

template<typename Rac, typename Coder> void decode_FLIF2_pass(Rac &rac, Image &image, const ColorRanges *ranges, std::vector<Tree> &forest, const int beginZL, const int endZL, int lastI)
{
    std::vector<Coder*> coders;
    for (int p = 0; p < image.numPlanes(); p++) {
        Ranges propRanges;
        initPropRanges(propRanges, *ranges, p);
        coders.push_back(new Coder(rac, propRanges, forest[p]));
    }

    if (beginZL == image.zooms()) {
      // special case: very left top pixel must be read first to get it all started
      SimpleSymbolCoder<FLIFBitChanceMeta, Rac, 24> metaCoder(rac);
      for (int p = 0; p < image.numPlanes(); p++) {
        image(p,0,0) = metaCoder.read_int(ranges->min(p), ranges->max(p));
      }
    }

    SimpleBitCoder<FLIFBitChanceParities,Rac> parityCoder(rac);

    decode_FLIF2_inner(coders, parityCoder, image, ranges, beginZL, endZL, lastI);

    for (int p = 0; p < image.numPlanes(); p++) {
        delete coders[p];
    }
}


/******************************************/
/*   General encoding/decoding            */
/******************************************/

template<typename BitChance, typename Rac> void encode_tree(Rac &rac, const ColorRanges *ranges, const std::vector<Tree> &forest, const int encoding)
{
    for (int p = 0; p < ranges->numPlanes(); p++) {
        Ranges propRanges;
        if (encoding==1) initPropRanges_scanlines(propRanges, *ranges, p);
        else initPropRanges(propRanges, *ranges, p);
        MetaPropertySymbolCoder<BitChance, Rac> metacoder(rac, propRanges);
//        forest[p].print(stdout);
        metacoder.write_tree(forest[p]);
    }
}
template<typename BitChance, typename Rac> void decode_tree(Rac &rac, const ColorRanges *ranges, std::vector<Tree> &forest, const int encoding)
{
    for (int p = 0; p < ranges->numPlanes(); p++) {
        Ranges propRanges;
        if (encoding==1) initPropRanges_scanlines(propRanges, *ranges, p);
        else initPropRanges(propRanges, *ranges, p);
        MetaPropertySymbolCoder<BitChance, Rac> metacoder(rac, propRanges);
        metacoder.read_tree(forest[p]);
//        forest[p].print(stdout);
    }
}


bool encode(const char* filename, Image &image, std::vector<std::string> transDesc, int encoding)
{
    f = fopen(filename,"w");
    RacOut rac(f);
    switch(encoding) {
        case 1: write_name(rac, "FLI1"); break;
        case 2: write_name(rac, "FLI2"); break;
        default: fprintf(stderr,"Unknown encoding: %i\n", encoding); return false;
    }
    SimpleSymbolCoder<FLIFBitChanceMeta, RacOut, 24> metaCoder(rac);
    int numPlanes = image.numPlanes();
    metaCoder.write_int(1, 16, numPlanes);
    metaCoder.write_int(1, 65536, image.cols());
    metaCoder.write_int(1, 65536, image.rows());
    printf("Input planes: ");
    for (int p = 0; p < numPlanes; p++) {
        assert(image.min(p) == 0);
        metaCoder.write_int(1, 16, ilog2(image.max(p)+1));
        printf("[%i] %i bpp (%i..%i) \t",p,ilog2(image.max(p)+1),image.min(p), image.max(p));
    }
    printf("\n");

    std::vector<const ColorRanges*> rangesList;
    std::vector<Transform*> transforms;
    rangesList.push_back(getRanges(image));
    for (unsigned int i=0; i<transDesc.size(); i++) {
        Transform *trans = create_transform(transDesc[i]);
        if (!trans->init(rangesList.back()) || !trans->process(rangesList.back(), image)) {
            fprintf(stderr, "Transform '%s' failed\n", transDesc[i].c_str());
        } else {
            printf("Doing transform '%s'\n", transDesc[i].c_str());
            rac.write(true);
            write_name(rac, transDesc[i]);
            trans->save(rangesList.back(), rac);
            rangesList.push_back(trans->meta(image, rangesList.back()));
            trans->data(image);
        }
    }
    rac.write(false);
    const ColorRanges* ranges = rangesList.back();
    grey.clear();
    for (int p = 0; p < ranges->numPlanes(); p++) grey.push_back((ranges->min(p)+ranges->max(p))/2);

    int mbits = 0;
    for (int p = 0; p < ranges->numPlanes(); p++) {
        if (ranges->max(p) > ranges->min(p)) {
          int nBits = ilog2((ranges->max(p) - ranges->min(p))*2-1)+1;
          if (nBits > mbits) mbits = nBits;
        }
    }
    const int bits = 10; // hardcoding things for 8 bit RGB (which means 9 bit IQ and 10 bit differences)
    if (mbits > bits) { printf("OOPS: %i > %i\n",mbits,bits); return false;}

    // two passes
    std::vector<Tree> forest(ranges->numPlanes(), Tree());
    RacDummy dummy;

    if (ranges->numPlanes() > 3) switch(encoding) {
        case 1: encode_scanlines_interpol_zero_alpha(image, ranges); break;
        case 2: encode_FLIF2_interpol_zero_alpha(image, ranges, image.zooms(), 0); break;
    }

    // not computing checksum until after transformations and potential zero-alpha changes
    uint32_t checksum = image.checksum();

    int roughZL = 0;
    if (encoding == 2) {
      roughZL = image.zooms() - NB_NOLEARN_ZOOMS-1;
      if (roughZL < 0) roughZL = 0;
      fprintf(stdout,"Encoding rough data\n");
      encode_FLIF2_pass<RacOut, FinalPropertySymbolCoder<FLIFBitChancePass2, RacOut, bits> >(rac, image, ranges, forest, image.zooms(), roughZL+1, 1);
    }

    fprintf(stdout,"Encoding data (pass 1)\n");
    switch(encoding) {
        case 1: encode_scanlines_pass<RacDummy, PropertySymbolCoder<FLIFBitChancePass1, RacDummy, bits> >(dummy, image, ranges, forest, TREE_LEARN_REPEATS); break;
        case 2: encode_FLIF2_pass<RacDummy, PropertySymbolCoder<FLIFBitChancePass1, RacDummy, bits> >(dummy, image, ranges, forest, roughZL, 0, TREE_LEARN_REPEATS); break;
    }

    fprintf(stdout,"Encoding tree\n");
    long fs = ftell(f);
    encode_tree<FLIFBitChanceTree, RacOut>(rac, ranges, forest, encoding);
    fprintf(stdout,"Rough data total: %li bytes.  Tree total: %li bytes.\n", fs, ftell(f)-fs);
    fprintf(stdout,"Encoding data (pass 2)\n");
    switch(encoding) {
        case 1: encode_scanlines_pass<RacOut, FinalPropertySymbolCoder<FLIFBitChancePass2, RacOut, bits> >(rac, image, ranges, forest, 1); break;
        case 2: encode_FLIF2_pass<RacOut, FinalPropertySymbolCoder<FLIFBitChancePass2, RacOut, bits> >(rac, image, ranges, forest, roughZL, 0, 1); break;
    }

    fprintf(stdout,"Encoding done\n");
    fprintf(stdout,"Writing checksum: %X\n", checksum);
    metaCoder.write_int(0, 0xFFFF, checksum / 0x10000);
    metaCoder.write_int(0, 0xFFFF, checksum & 0xFFFF);
    rac.flush();
    fclose(f);

    for (int i=transforms.size()-1; i>=0; i--) {
        delete transforms[i];
    }
    transforms.clear();
    for (unsigned int i=0; i<rangesList.size(); i++) {
        delete rangesList[i];
    }
    rangesList.clear();
    return true;
}



bool decode(const char* filename, Image &image, int lastI)
{
    image.reset();

    f = fopen(filename,"r");
    RacIn rac(f);
    int encoding=0;

    std::string str = read_name(rac);
    if (str == "FLI1") {
        encoding=1;
    } else if (str == "FLI2") {
        encoding=2;
    } else {
        fprintf(stderr,"Unknown magic '%s'\n", str.c_str());
        return false;
    }

    SimpleSymbolCoder<FLIFBitChanceMeta, RacIn, 24> metaCoder(rac);
    int numPlanes = metaCoder.read_int(1, 16);
    int width = metaCoder.read_int(1, 65536);
    int height = metaCoder.read_int(1, 65536);
    image.init(width, height, 0, 0, 0);
    for (int p = 0; p < numPlanes; p++) {
        int subSampleR = 1;
        int subSampleC = 1;
        int min = 0;
        int max = (1 << metaCoder.read_int(1, 16)) - 1;
        image.add_plane(min, max, subSampleR, subSampleC);
        printf("plane %i: %i bits per pixel (%i..%i)\n",p,ilog2(image.max(p)+1),image.min(p), image.max(p));
    }

    std::vector<const ColorRanges*> rangesList;
    std::vector<Transform*> transforms;
    rangesList.push_back(getRanges(image));
    while (rac.read()) {
        std::string desc = read_name(rac);
        Transform *trans = create_transform(desc);
        if (!trans) {
            fprintf(stderr,"Unknown transformation '%s'\n", desc.c_str());
            return false;
        }
        if (!trans->init(rangesList.back())) {
            fprintf(stderr,"Transformation '%s' failed\n", desc.c_str());
            return false;
        }
        printf("Doing transform '%s'\n", desc.c_str());
        trans->load(rangesList.back(), rac);
        rangesList.push_back(trans->meta(image, rangesList.back()));
        transforms.push_back(trans);
    }
    const ColorRanges* ranges = rangesList.back();
    grey.clear();
    for (int p = 0; p < ranges->numPlanes(); p++) grey.push_back((ranges->min(p)+ranges->max(p))/2);

    for (int p = 0; p < numPlanes; p++) {
        if (ranges->min(p) >= ranges->max(p)) {
             printf("Constant plane %i at color value %i\n",p,ranges->min(p));
             for (ColorVal& x : image(p).data) x=ranges->min(p);
        }
    }

    int mbits = 0;
    for (int p = 0; p < ranges->numPlanes(); p++) {
        if (ranges->max(p) > ranges->min(p)) {
          int nBits = ilog2((ranges->max(p) - ranges->min(p))*2-1)+1;
          if (nBits > mbits) mbits = nBits;
        }
    }
    const int bits = 10;
    if (mbits > bits) { printf("OOPS: %i > %i\n",mbits,bits); return false;}


    std::vector<Tree> forest(ranges->numPlanes(), Tree());

    int roughZL = 0;
    if (encoding == 2) {
      roughZL = image.zooms() - NB_NOLEARN_ZOOMS-1;
      if (roughZL < 0) roughZL = 0;
      fprintf(stdout,"Decoding rough data\n");
      decode_FLIF2_pass<RacIn, FinalPropertySymbolCoder<FLIFBitChancePass2, RacIn, bits> >(rac, image, ranges, forest, image.zooms(), roughZL+1, -1);
    }
    if (encoding == 2 && lastI < -2) {
      printf("Not decoding tree\n");
    } else {
      fprintf(stdout,"Decoding tree\n");
      decode_tree<FLIFBitChanceTree, RacIn>(rac, ranges, forest, encoding);
    }
    switch(encoding) {
        case 1: fprintf(stdout,"Decoding data (scanlines)\n");
                decode_scanlines_pass<RacIn, FinalPropertySymbolCoder<FLIFBitChancePass2, RacIn, bits> >(rac, image, ranges, forest);
                break;
        case 2: fprintf(stdout,"Decoding data (FLIF2)\n");
                decode_FLIF2_pass<RacIn, FinalPropertySymbolCoder<FLIFBitChancePass2, RacIn, bits> >(rac, image, ranges, forest, roughZL, 0, lastI);
                break;
    }
    fprintf(stdout,"Decoding done, read %ld bytes\n",ftell(f));


    if (lastI < 0) {
      uint32_t checksum = image.checksum();
      fprintf(stdout,"Computed checksum: %X\n", checksum);
      uint32_t checksum2 = metaCoder.read_int(0, 0xFFFF);
      checksum2 *= 0x10000;
      checksum2 += metaCoder.read_int(0, 0xFFFF);
      fprintf(stdout,"Read checksum: %X\n", checksum2);
      if (checksum != checksum2) printf("\nCORRUPTION DETECTED!\n");
    } else {
      fprintf(stdout,"Not checking checksum, lossy partial decoding was chosen.\n");
    }

    for (int i=transforms.size()-1; i>=0; i--) {
        transforms[i]->invData(image);
        delete transforms[i];
    }
    transforms.clear();


    for (unsigned int i=0; i<rangesList.size(); i++) {
        delete rangesList[i];
    }
    rangesList.clear();

    fclose(f);
    return true;
}


int main(int argc, char **argv)
{
    Image image;
    if (argc == 3) {
        image.load(argv[1]);
        std::vector<std::string> desc;
        desc.push_back("YIQ");  // convert RGB(A) to YIQ(A)
        desc.push_back("BND");  // get the bounds of the color spaces
        desc.push_back("PLT");  // try palette
        desc.push_back("ACB");  // try auto color buckets
        encode(argv[2], image, desc, 2);
    } else if (argc == 4) {
        decode(argv[2], image, -1);
        image.save(argv[3]);
    } else if (argc == 5) {
        decode(argv[3], image, strtol(argv[2],NULL,0));
        image.save(argv[4]);
    } else {
        fprintf(stderr, "Usage: %s [-d [level]] source dest\n", argv[0]);
        return 1;
    }
    return 0;
}
