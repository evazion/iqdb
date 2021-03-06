/***************************************************************************\
    imgdb.cpp - iqdb library implementation

    Copyright (C) 2008 piespy@gmail.com

    Originally based on imgSeek code, these portions
    Copyright (C) 2003 Ricardo Niederberger Cabral.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\**************************************************************************/

#include <sys/mman.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <iqdb/debug.h>
#include <iqdb/imgdb.h>
#include <iqdb/imglib.h>
#include <iqdb/haar_signature.h>
#include <iqdb/sqlite_db.h>

namespace imgdb {

void bucket_set::add(const HaarSignature &sig, imageId iqdb_id) {
  eachBucket(sig, [&](auto& bucket) {
    bucket.push_back(iqdb_id);
  });
}

void bucket_set::remove(const HaarSignature &sig, imageId iqdb_id) {
  eachBucket(sig, [&](auto& bucket) {
    // https://en.wikipedia.org/wiki/Erase-remove_idiom
    bucket.erase(std::remove(bucket.begin(), bucket.end(), iqdb_id), bucket.end());
  });
}

bucket_t& bucket_set::at(int color, int coef) {
  const int sign = coef < 0;
  return buckets[color][sign][abs(coef)];
}

void bucket_set::eachBucket(const HaarSignature &sig, std::function<void(bucket_t&)> func) {
  for (int c = 0; c < sig.num_colors(); c++) {
    for (int i = 0; i < NUM_COEFS; i++) {
      const int coef = sig.sig[c][i];
      auto& bucket = at(c, coef);
      func(bucket);
    }
  }
}

void dbSpaceImpl::addImage(imageId post_id, const HaarSignature& haar) {
  removeImage(post_id);
  int iqdb_id = sqlite_db_->addImage(post_id, haar);
  addImageInMemory(iqdb_id, post_id, haar);

  DEBUG("Added post #{} to memory and database (iqdb={} haar={}).\n", post_id, iqdb_id, haar.to_string());
}

void dbSpaceImpl::addImageInMemory(imageId iqdb_id, imageId post_id, const HaarSignature& haar) {
  if ((size_t)iqdb_id >= m_info.size()) {
    DEBUG("Growing m_info array (size={}).\n", m_info.size());
    m_info.resize(iqdb_id + 50000);
  }

  imgbuckets.add(haar, iqdb_id);

  image_info& info = m_info.at(iqdb_id);
  info.id = post_id;
  info.avgl.v[0] = static_cast<Score>(haar.avglf[0]);
  info.avgl.v[1] = static_cast<Score>(haar.avglf[1]);
  info.avgl.v[2] = static_cast<Score>(haar.avglf[2]);
}

void dbSpaceImpl::loadDatabase(std::string filename) {
  sqlite_db_ = std::make_unique<SqliteDB>(filename);
  m_info.clear();
  imgbuckets = bucket_set();

  sqlite_db_->eachImage([&](const auto& image) {
    addImageInMemory(image.id, image.post_id, image.haar());

    if (image.id % 250000 == 0) {
      INFO("Loaded image {} (post #{})...\n", image.id, image.post_id);
    }
  });

  INFO("Loaded {} images from {}.\n", getImgCount(), filename);
}

bool dbSpaceImpl::isDeleted(imageId iqdb_id) {
  return !m_info.at(iqdb_id).avgl.v[0];
}

std::optional<Image> dbSpaceImpl::getImage(imageId post_id) {
  return sqlite_db_->getImage(post_id);
}

sim_vector dbSpace::queryFromBlob(const std::string blob, int numres) {
  HaarSignature signature = HaarSignature::from_file_content(blob);
  return queryFromSignature(signature, numres);
}

sim_vector dbSpaceImpl::queryFromSignature(const HaarSignature &signature, size_t numres) {
  Score scale = 0;
  std::vector<Score> scores(m_info.size(), 0);
  std::priority_queue<sim_value> pqResults; /* results priority queue; largest at top */
  sim_vector V; /* output results */

  DEBUG("Querying signature={} json={}\n", signature.to_string(), signature.to_json());

  // Luminance score (DC coefficient).
  for (size_t i = 0; i < scores.size(); i++) {
    auto image_info = m_info[i];
    Score s = 0;

    for (int c = 0; c < signature.num_colors(); c++) {
      s += weights[0][c] * std::abs(image_info.avgl.v[c] - static_cast<Score>(signature.avglf[c]));
    }

    scores[i] = s;
  }

  for (int c = 0; c < signature.num_colors(); c++) {
    for (int b = 0; b < NUM_COEFS; b++) { // for every coef on a sig
      const int coef = signature.sig[c][b];
      auto &bucket = imgbuckets.at(c, coef);

      if (bucket.empty())
        continue;

      const int w = imgBin.bin[abs(coef)];
      Score weight = weights[w][c];
      scale -= weight;

      for (auto index : bucket) {
        scores[index] -= weight;
      }
    }
  }

  // Fill up the numres-bounded priority queue (largest at top):
  iqdbId i = 0;
  for (; pqResults.size() < numres && i < scores.size(); i++) {
    if (!isDeleted(i))
      pqResults.emplace(i, scores[i]);
  }

  for (; i < scores.size(); i++) {
    if (!isDeleted(i) && scores[i] < pqResults.top().score) {
      pqResults.pop();
      pqResults.emplace(i, scores[i]);
    }
  }

  if (scale != 0)
    scale = static_cast<Score>(1.0) / scale;

  while (!pqResults.empty()) {
    auto value = pqResults.top();
    value.id = m_info[value.id].id; // XXX replace iqdb id with post id
    value.score = value.score * 100 * scale;

    V.push_back(value);
    pqResults.pop();
  }

  std::reverse(V.begin(), V.end());
  return V;
}

void dbSpaceImpl::removeImage(imageId post_id) {
  auto image = sqlite_db_->getImage(post_id);
  if (image == std::nullopt) {
    WARN("Couldn't remove post #{}; post not in sqlite database.\n", post_id);
    return;
  }

  imgbuckets.remove(image->haar(), image->id);
  m_info.at(image->id).avgl.v[0] = 0;
  sqlite_db_->removeImage(post_id);

  DEBUG("Removed post #{} from memory and database.\n", post_id);
}

/*
Score dbSpaceCommon::calcAvglDiff(imageId id1, imageId id2) {
  // return the average luminance difference

  // are images on db ?
  lumin_native avgl1, avgl2;
  getImgAvgl(id1, avgl1);
  getImgAvgl(id2, avgl2);
  return std::abs(avgl1.v[0] - avgl2.v[0]) + std::abs(avgl1.v[1] - avgl2.v[1]) + std::abs(avgl1.v[2] - avgl2.v[2]);
}

Score dbSpaceCommon::calcSim(imageId id1, imageId id2, bool ignore_color) {
  // use it to tell the content-based difference between two images
  ImgData dsig1, dsig2;
  getImgDataByID(id1, &dsig1);
  getImgDataByID(id2, &dsig2);

  Idx *const sig1[3] = {dsig1.sig1, dsig1.sig2, dsig1.sig3};
  Idx *const sig2[3] = {dsig2.sig1, dsig2.sig2, dsig2.sig3};

  Score score = 0, scale = 0;
  lumin_native avgl1, avgl2;
  image_info::avglf2i(dsig1.avglf, avgl1);
  image_info::avglf2i(dsig2.avglf, avgl2);

  int cnum = ignore_color || is_grayscale(avgl1) || is_grayscale(avgl2) ? 1 : 3;

  for (int c = 0; c < cnum; c++)
    score += DScSc(2 * ((DScore)weights[0][c]) * std::abs(avgl1.v[c] - avgl2.v[c]));

  for (int c = 0; c < cnum; c++) {
    std::sort(sig1[c] + 0, sig1[c] + NUM_COEFS);
    std::sort(sig2[c] + 0, sig2[c] + NUM_COEFS);

    for (int b1 = 0, b2 = 0; b1 < NUM_COEFS || b2 < NUM_COEFS;) {
      int ind1 = b1 == NUM_COEFS ? std::numeric_limits<int>::max() : sig1[c][b1];
      int ind2 = b2 == NUM_COEFS ? std::numeric_limits<int>::max() : sig2[c][b2];

      Score weight = weights[imgBin[std::abs(ind1 < ind2 ? ind1 : ind2)]][c];
      scale -= weight;

      if (ind1 == ind2)
        score -= weight;

      b1 += ind1 <= ind2;
      b2 += ind2 <= ind1;
    }
  }

  scale = ((DScore)MakeScore(1)) * MakeScore(1) / scale;
  return DScSc(((DScore)score) * 100 * scale);
}
*/

size_t dbSpaceImpl::getImgCount() {
  return m_info.size();
}

dbSpace::dbSpace() {}
dbSpace::~dbSpace() {}

dbSpaceImpl::dbSpaceImpl(std::string filename) : sqlite_db_(nullptr) {
  loadDatabase(filename);
}

dbSpaceImpl::~dbSpaceImpl() {
}

} // namespace imgdb
