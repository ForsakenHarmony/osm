/**
 *  OSM
 *  Copyright (C) 2021  Pavel Smokotnin

 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.

 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.

 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "nyquistseriesrenderer.h"
#include "common/notifier.h"
#include "../nyquistplot.h"

namespace chart {

NyquistSeriesRenderer::NyquistSeriesRenderer() : XYSeriesRenderer(),
    m_pointsPerOctave(0),
    m_coherenceThreshold(0), m_coherence(false)
{
    m_program.addShaderFromSourceFile(QOpenGLShader::Vertex, ":/nyquist.vert");
    m_program.addShaderFromSourceFile(QOpenGLShader::Geometry, ":/nyquist.geom");
    m_program.addShaderFromSourceFile(QOpenGLShader::Fragment, ":/nyquist.frag");
    if (!m_program.link()) {
        emit Notifier::getInstance()->newMessage("NyquistSeriesRenderer", m_program.log());
    }

    m_widthUniform  = m_program.uniformLocation("width");
    m_colorUniform  = m_program.uniformLocation("m_color");
    m_matrixUniform = m_program.uniformLocation("matrix");
    m_screenUniform = m_program.uniformLocation("screen");
    m_coherenceThresholdU = m_program.uniformLocation("coherenceThreshold");
    m_coherenceAlpha      = m_program.uniformLocation("coherenceAlpha");

}

void NyquistSeriesRenderer::renderSeries()
{
    if (!m_source->active() || !m_source->size())
        return;

    //max octave count: 11
    unsigned int maxBufferSize = m_pointsPerOctave * 12 * 12, i = 0, verticiesCount = 0;
    if (m_vertices.size() != maxBufferSize) {
        m_vertices.resize(maxBufferSize);
        m_refreshBuffers = true;
    }

    struct NyquistSplineValue {
        complex m_phase = 0;
        float m_magnitude = 0;

        NyquistSplineValue(complex phase, float magnitude) : m_phase(phase), m_magnitude(magnitude) {}
        NyquistSplineValue(const NyquistSplineValue &right)
        {
            m_phase = right.m_phase;
            m_magnitude = right.m_magnitude;
        };
        NyquistSplineValue(NyquistSplineValue &&right) noexcept
        {
            m_phase = std::move(right.m_phase);
            m_magnitude = std::move(right.m_magnitude);
        };

        NyquistSplineValue &operator=(const NyquistSplineValue &rh)
        {
            m_phase = rh.m_phase;
            m_magnitude = rh.m_magnitude;
            return *this;
        }

        NyquistSplineValue &operator+=(const complex &rh)
        {
            m_phase += rh;
            return *this;
        }

        NyquistSplineValue &operator+=(const float &rh)
        {
            m_magnitude += rh;
            return *this;
        }

        void reset()
        {
            m_phase = 0;
            m_magnitude = 0;
        }

    };
    NyquistSplineValue value(0, 0);
    float coherence = 0.f;

    auto accumulate = [&value, &coherence, this] (const unsigned int &i) {
        value += m_source->phase(i);
        value += m_source->magnitudeRaw(i);
        coherence += m_source->coherence(i);
    };

    auto beforeSpline = [] (const auto * value, auto, const auto & count) {
        complex c = value->m_phase / count;
        c /= c.abs();
        c *= value->m_magnitude / count;
        return c;
    };

    auto collected = [ &, this] (const float &, const float &, const complex ac[4], const float c[4]) {
        if (i > maxBufferSize) {
            qCritical("out of range");
            return;
        }

        m_vertices[i + 0] = ac[0].real;
        m_vertices[i + 1] = ac[1].real;
        m_vertices[i + 2] = ac[2].real;
        m_vertices[i + 3] = ac[3].real;

        m_vertices[i + 4] = ac[0].imag;
        m_vertices[i + 5] = ac[1].imag;
        m_vertices[i + 6] = ac[2].imag;
        m_vertices[i + 7] = ac[3].imag;
        std::memcpy(m_vertices.data() + i + 8,  c, 4 * 4);
        verticiesCount ++;
        i += 12;

        value.reset();
        coherence = 0.f;
    };

    iterateForSpline<NyquistSplineValue, complex>(m_pointsPerOctave, &value, &coherence, accumulate, collected,
                                                  beforeSpline);

    {
        m_program.setUniformValue(m_matrixUniform, m_matrix);
        m_program.setUniformValue(m_screenUniform, m_width, m_height);
        m_program.setUniformValue(m_widthUniform, m_weight * m_retinaScale);
    }
    m_program.setUniformValue(m_coherenceThresholdU, m_coherenceThreshold);
    m_program.setUniformValue(m_coherenceAlpha, m_coherence);

    if (m_refreshBuffers) {
        m_openGLFunctions->glGenBuffers(1, &m_vertexBufferId);
        m_openGLFunctions->glGenVertexArrays(1, &m_vertexArrayId);
    }

    m_openGLFunctions->glBindVertexArray(m_vertexArrayId);
    m_openGLFunctions->glBindBuffer(GL_ARRAY_BUFFER, m_vertexBufferId);

    if (m_refreshBuffers) {
        m_openGLFunctions->glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * maxBufferSize, nullptr, GL_DYNAMIC_DRAW);
        m_openGLFunctions->glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(GLfloat),
                                                 reinterpret_cast<const void *>(0));
        m_openGLFunctions->glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(GLfloat),
                                                 reinterpret_cast<const void *>(4 * sizeof(GLfloat)));
        m_openGLFunctions->glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(GLfloat),
                                                 reinterpret_cast<const void *>(8 * sizeof(GLfloat)));
    }
    m_openGLFunctions->glBufferSubData(GL_ARRAY_BUFFER, 0, 12 * sizeof(GLfloat) * verticiesCount, m_vertices.data());

    m_openGLFunctions->glEnableVertexAttribArray(0);
    m_openGLFunctions->glEnableVertexAttribArray(1);
    m_openGLFunctions->glEnableVertexAttribArray(2);
    m_openGLFunctions->glDrawArrays(GL_POINTS, 0, verticiesCount);
    m_openGLFunctions->glDisableVertexAttribArray(2);
    m_openGLFunctions->glDisableVertexAttribArray(1);
    m_openGLFunctions->glDisableVertexAttribArray(0);

    m_refreshBuffers = false;
}

void NyquistSeriesRenderer::synchronize(QQuickFramebufferObject *item)
{
    XYSeriesRenderer::synchronize(item);

    if (auto *plot = dynamic_cast<NyquistPlot *>(m_item->parent())) {
        m_pointsPerOctave = plot->pointsPerOctave();
        m_coherence = plot->coherence();
        m_coherenceThreshold = plot->coherenceThreshold();
    }
}

void NyquistSeriesRenderer::updateMatrix()
{
    m_matrix = {};
    m_matrix.ortho(m_xMin, m_xMax, m_yMax, m_yMin, -1, 1);
}

Source *NyquistSeriesRenderer::source() const
{
    return m_source;
}

} // namespace chart
