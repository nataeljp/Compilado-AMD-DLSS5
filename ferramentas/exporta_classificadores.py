# Exporta os classificadores da mascara de desenvolvedor (bin\ml) para ONNX e confere contra a referencia.
#  - cena.onnx: LR-ASPP MobileNetV3 Large (torchvision, BSD-3; COCO com os rotulos VOC). Entrada 1x3x360x640 RGB 0..1
#    (sRGB; a normalizacao ImageNet vai no grafo); saida 1x3x360x640: probabilidade de pessoa, animal e objeto (soma das
#    classes VOC de cada grupo, depois do softmax).
#  - partes.onnx: MediaPipe selfie multiclass 256x256 (Apache-2.0), convertido do tflite (tf2onnx). Entrada 1x256x256x3
#    RGB em -1..1; saida 1x256x256x6 logits: fundo, cabelo, pele do corpo, pele do rosto, roupa, outros (acessorios).
# uso: python exporta_classificadores.py <pasta_saida> <selfie_multiclass_256x256.tflite> <foto_teste>
import subprocess, sys
from pathlib import Path
import numpy as np
import torch
from PIL import Image
from torchvision.models.segmentation import lraspp_mobilenet_v3_large, LRASPP_MobileNet_V3_Large_Weights
import onnxruntime as ort

out, tfl, foto = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
out.mkdir(parents=True, exist_ok=True)
ANIMAIS = [3, 8, 10, 12, 13, 17]                           # passaro, gato, vaca, cachorro, cavalo, ovelha
OBJETOS = [1, 2, 4, 5, 6, 7, 9, 11, 14, 16, 18, 19, 20]   # aviao, bicicleta, barco, garrafa, onibus, carro, cadeira, mesa,
                                                           # moto, planta, sofa, trem, tela


class Cena(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.m = lraspp_mobilenet_v3_large(weights=LRASPP_MobileNet_V3_Large_Weights.DEFAULT).eval()
        self.register_buffer('media', torch.tensor([0.485, 0.456, 0.406]).view(1, 3, 1, 1))
        self.register_buffer('desvio', torch.tensor([0.229, 0.224, 0.225]).view(1, 3, 1, 1))

    def forward(self, x):
        p = torch.softmax(self.m((x - self.media) / self.desvio)['out'], 1)
        return torch.cat([p[:, 15:16], p[:, ANIMAIS].sum(1, keepdim=True), p[:, OBJETOS].sum(1, keepdim=True)], 1)


cena = Cena().eval()
x = torch.from_numpy(np.asarray(Image.open(foto).convert('RGB').resize((640, 360)), np.float32).transpose(2, 0, 1)[None] / 255)
with torch.no_grad():
    ref = cena(x).numpy()
torch.onnx.export(cena, x, str(out / 'cena.onnx'), input_names=['imagem'], output_names=['prob'], opset_version=17,
                  dynamo=False)
s = ort.InferenceSession(str(out / 'cena.onnx'), providers=['DmlExecutionProvider'])
o = s.run(None, {'imagem': x.numpy()})[0]
print('cena: erro max %.2e, pessoa %.3f' % (np.abs(o - ref).max(), (o[0, 0] > .5).mean()))

subprocess.run([sys.executable, '-m', 'tf2onnx.convert', '--tflite', tfl, '--output', str(out / 'partes.onnx'),
                '--opset', '17'], check=True, capture_output=True)
import mediapipe as mp
from mediapipe.tasks import python as mpt
from mediapipe.tasks.python import vision
seg = vision.ImageSegmenter.create_from_options(vision.ImageSegmenterOptions(
    base_options=mpt.BaseOptions(model_asset_path=tfl), output_category_mask=False, output_confidence_masks=True))
r = np.asarray(Image.open(foto).convert('RGB').resize((256, 256)))
mref = np.stack([np.asarray(c.numpy_view()).squeeze() for c in
                 seg.segment(mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(r))).confidence_masks], -1)
s = ort.InferenceSession(str(out / 'partes.onnx'), providers=['DmlExecutionProvider'])
z = s.run(None, {s.get_inputs()[0].name: (r / 127.5 - 1).astype(np.float32)[None]})[0][0]
z = np.exp(z - z.max(-1, keepdims=True))
z /= z.sum(-1, keepdims=True)
print('partes: erro max vs mediapipe %.2e' % np.abs(z - mref).max())
